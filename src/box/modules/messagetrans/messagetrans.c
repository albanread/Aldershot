/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/Internat/MsgTrans: s.MsgTrans).
 */

/* messagetrans.c -- MessageTrans, reimplemented.
 *
 * Written from RISC OS's MessageTrans source (Internat/MsgTrans, s/MsgTrans
 * and its Doc).  Its SWIs are register-level, as its callers use them.
 * What they leave in memory is what MessageTrans leaves.
 *
 *   - A client's descriptor holds its magic word, flags, the file's data (a
 *     word of length + 4, then the file) and the filename.  A descriptor
 *     opened with R2 = 0 holds "FAST", a flag and a pointer instead.  The
 *     pointer is to a 32-byte proxy in the RMA that MessageTrans keeps on
 *     its list.
 *   - A file in ResourceFS is used where it lies.  Any other file is loaded
 *     into the RMA or the client's buffer.
 *   - A lookup is read from the file byte by byte as MessageTrans reads it.
 *     "#" lines are skipped.  Alternatives are separated by "/" and by line.
 *     "?" matches any character.  "token:default" is understood.  %0-%3 and
 *     %% are substituted, and escapes are kept.  A token the file lacks is
 *     looked up in the Global messages.
 *   - Errors are built in MessageTrans's 16 internal buffers (4 for
 *     background work), cycling as it cycles them.  Its own texts are
 *     looked up in its own Messages file.
 *
 * A file in ResourceFS (modules/resourcefs) is used where it lies.  Any
 * other name goes to OS_File, which FileSwitch answers.
 *
 * Not kept are the hash tables that it builds to go faster.  Its flags for
 * them are internal.  The kernel's help dictionary is the sources' minimum
 * one (Internat/Messages, Global MinDict: token 1 a new line, 2
 * "Syntax: *").  Those are the tokens that the modules' Messages files in
 * the sources use.  A RISC OS build makes a larger dictionary and
 * tokenises its files with it.
 */
#include <string.h>

#include "messagetrans.h"
#include "resourcefs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define SERVICE_RESOURCEFS_STARTED 0x59u
#define SERVICE_RESOURCEFS_DYING   0x5Au
#define SERVICE_MESSAGE_FILE_CLOSED 0x5Eu
#define SERVICE_TERRITORY_STARTED  0x75u
#define SERVICE_MODULE_POST_INIT   0xDAu

#define ERR_SYNTAX       0xAC0u
#define ERR_TOKEN        0xAC2u
#define ERR_RECURSE      0xAC3u
#define ERR_BAD_DESC     0xAC4u
#define ERR_BUFF_OVERFLOW 0x2C1u

#define MAGIC_FAST 0x54534146u      /* "FAST" */
#define MAGIC_SLOW 0x574F4C53u      /* "SLOW" */

/* Descriptor flags */
#define FLG_INRESOURCEFS 0x00000001u
#define FLG_OURBUFFER    0x00000002u
#define FLG_ADDTOLIST    0x00000004u
#define FLG_NEW_API      0x04000000u
#define FLG_WORTH_HASHING 0x08000000u
#define FLG_FREEBLOCK    0x20000000u
#define FLG_PROXY        0x40000000u
#define FLG_HIDDEN       0xFE000000u

/* A proxy, 32 bytes: link, flags, file pointer, filename, hash, the
 * client's block, use count, reserved.  A client's block, 16: magic,
 * flags, file pointer or proxy, filename. */
#define F_LINK 0
#define F_FLAGS 4
#define F_FILEPTR 8
#define F_FILENAME 12
#define F_CLNT_BLOCK 20
#define F_USE_COUNT 24

#define BUFFER_SIZE 256u
#define FOREGROUND_BUFFERS 16u
#define IRQ_BUFFERS 4u

struct workspace {
    uint32_t link_header;           /* proxies on the list */
    uint32_t fg_next, irq_next, threadness;
    uint32_t own[5];                /* [0] 0 or -> [1..4], our Messages */
    uint32_t global[5];             /* likewise, the Global messages */
    uint32_t dictionary;
    uint8_t buffers[FOREGROUND_BUFFERS + IRQ_BUFFERS][BUFFER_SIZE];
};

static struct workspace *ws(void)
{
    return ros_ptr(ros_ld32(messagetrans_module.private_word));
}

static uint32_t L(uint32_t a) { return ros_ld32(a); }
static void S(uint32_t a, uint32_t v) { ros_st32(a, v); }
static uint32_t B(uint32_t a) { return ros_ld8(a); }

static uint32_t own_block(void)
{
    return ws()->own[0];
}

static uint32_t arena_of(const void *p)
{
    return ros_addr(p);
}

/* ---- internal buffers ------------------------------------------------------------ */

/* The kernel's AllocateInternalBuffer: background work (IRQsema set) gets
 * the four IRQ buffers, the rest the sixteen, each used in turn. */
static uint32_t internal_buffer(void)
{
    struct workspace *w = ws();
    unsigned i;
    if (L(ROS_ZP_IRQSEMA)) {
        i = FOREGROUND_BUFFERS + w->irq_next;
        w->irq_next = (w->irq_next + 1) % IRQ_BUFFERS;
    } else {
        i = w->fg_next;
        w->fg_next = (w->fg_next + 1) % FOREGROUND_BUFFERS;
    }
    return arena_of(w->buffers[i]);
}

/* ---- files ------------------------------------------------------------------------ */

struct info {
    uint32_t flags;                 /* FLG_INRESOURCEFS, or 0 */
    uint32_t data;                  /* in ResourceFS: the word before the data */
    uint32_t size;                  /* the file's length + 4 */
};

static os_error *error_lookup(uint32_t block, uint32_t errnum, const char *token, uint32_t p0);

/* The kernel's internal_fileinfo: in ResourceFS, the data where it lies;
 * otherwise the file's length, from OS_File. */
static os_error *file_info(uint32_t name, struct info *f)
{
    struct workspace *w = ws();
    if (w->threadness >= 2)
        return ros_error(ERR_RECURSE, "Recursion in MessageTrans");
    w->threadness++;
    os_error *e = NULL;
    uint32_t data = ros_resourcefs_find(ros_ptr(name));
    if (data) {
        f->flags = FLG_INRESOURCEFS;
        f->data = data;
        f->size = L(data);
    } else {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 5, c.r[1] = name;
        ros_swi(&c, XOS_File);
        if (c.v)
            e = ros_ptr(c.r[0]);
        else if (c.r[0] != 1)
            e = error_lookup(0, 0xD6, "NoFile", name);      /* global: "File '%0' not found" */
        f->flags = 0;
        f->data = 0;
        f->size = c.r[4] + 4;
    }
    w->threadness--;
    return e;
}

/* The file's data into buffer: its length + 4, then the data. */
static os_error *load_into(uint32_t name, const struct info *f, uint32_t buffer)
{
    S(buffer, f->size);
    if (f->flags & FLG_INRESOURCEFS) {
        memcpy(ros_ptr(buffer + 4), ros_ptr(f->data + 4), f->size - 4);
        return NULL;
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 255, c.r[1] = name, c.r[2] = buffer + 4, c.r[3] = 0;   /* OS_File load, here */
    ros_swi(&c, XOS_File);
    return c.v ? (os_error *)ros_ptr(c.r[0]) : NULL;
}

/* A descriptor's own block: a proxy's, when it has one. */
static uint32_t translate(uint32_t d)
{
    if (d && (L(d + F_FLAGS) & FLG_PROXY))
        d = L(d + F_FILEPTR);
    return d;
}

static void free_data(uint32_t d)
{
    uint32_t flags = L(d + F_FLAGS), data = L(d + F_FILEPTR);
    if (flags & FLG_FREEBLOCK)
        ros_rma_free(ros_ptr(d));
    if ((flags & FLG_OURBUFFER) && data)
        ros_rma_free(ros_ptr(data));
}

/* ---- OpenFile and CloseFile ------------------------------------------------------ */

static os_error *close_file(uint32_t client)
{
    uint32_t d = translate(client);
    if (!d)
        return error_lookup(own_block(), ERR_BAD_DESC, "BadDesc", 0);
    struct workspace *w = ws();
    for (uint32_t prev = arena_of(&w->link_header) - F_LINK, b; (b = L(prev + F_LINK)) != 0;
         prev = b) {
        if (b == d) {
            S(prev + F_LINK, L(d + F_LINK));
            free_data(d);
            break;
        }
    }
    /* The kernel tests the block's first word here against the flag bits.
     * That word is the magic, or "FAST" after a proxied open.  So do we. */
    if (!(L(client + 0) & (FLG_PROXY | FLG_NEW_API)))
        return NULL;                    /* left, so old programs can still look up */
    S(client + 0, 0);
    S(client + F_FLAGS, 0);
    S(client + F_FILEPTR, 0);
    return NULL;
}

static os_error *open_file(uint32_t desc, uint32_t name, uint32_t buffer, uint32_t magic)
{
    /* Already one of ours? */
    if (L(desc) == MAGIC_FAST) {
        uint32_t flags = L(desc + F_FLAGS);
        int reopen = 1;
        if (flags & FLG_PROXY) {
            uint32_t proxy = L(desc + F_FILEPTR), b = ws()->link_header;
            while (b && b != proxy)
                b = L(b + F_LINK);
            if (!b) {
                reopen = 0;             /* not on the list: start afresh */
            } else if (L(proxy + F_CLNT_BLOCK) == desc &&
                       (L(proxy + F_FLAGS) & FLG_INRESOURCEFS) &&
                       L(proxy + F_FILENAME) == name) {
                if (L(proxy + F_FILEPTR))
                    return NULL;        /* nothing to do */
                struct info f;
                os_error *e = file_info(name, &f);
                if (!e)
                    S(proxy + F_FILEPTR, f.data);
                return e;
            }
        }
        if (reopen)
            close_file(desc);
    }

    struct info f = { 0, 0, 0 };
    os_error *e;
    if (name == 0)
        f.size = L(buffer);             /* the data is in the buffer already */
    else if ((e = file_info(name, &f)))
        return e;

    uint32_t flags = f.flags;
    if (magic == MAGIC_FAST) {
        flags |= FLG_NEW_API | FLG_WORTH_HASHING;
        S(desc + F_CLNT_BLOCK, 0);
        S(desc + F_USE_COUNT, 0);
    } else if (magic == MAGIC_SLOW) {
        flags &= ~FLG_WORTH_HASHING;
    } else if (buffer == 0) {
        flags |= FLG_WORTH_HASHING;
    }
    S(desc + F_FLAGS, flags);
    S(desc + F_FILENAME, name);
    S(desc + 0, magic);

    if (buffer) {                       /* the client's buffer, from any filing system */
        S(desc + F_FLAGS, flags & ~FLG_INRESOURCEFS);
        S(desc + F_FILEPTR, buffer);
        return name ? load_into(name, &f, buffer) : NULL;
    }

    /* A proxy in the RMA, on our list */
    uint32_t proxy = ros_addr(ros_rma_alloc(32));
    if (!proxy)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    uint32_t old_ptr = L(desc + F_FILEPTR);
    S(desc + 0, MAGIC_FAST);
    S(desc + F_FLAGS, flags | FLG_PROXY);
    S(desc + F_FILEPTR, proxy);
    S(proxy + F_LINK, 0);
    S(proxy + F_FLAGS, flags | FLG_FREEBLOCK);
    S(proxy + F_FILEPTR, old_ptr);
    S(proxy + F_FILENAME, name);
    S(proxy + 16, 0);
    S(proxy + F_CLNT_BLOCK, desc);
    S(proxy + F_USE_COUNT, 0);
    S(proxy + 28, 0);
    if (flags & FLG_INRESOURCEFS) {
        S(proxy + F_FILEPTR, f.data);
    } else {
        uint32_t data = ros_addr(ros_rma_alloc(f.size));
        if (!data)
            e = ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
        else if ((e = load_into(name, &f, data)))
            ros_rma_free(ros_ptr(data));
        if (e)
            return e;
        S(proxy + F_FLAGS, L(proxy + F_FLAGS) | FLG_OURBUFFER);
        S(proxy + F_FILEPTR, data);
    }
    S(proxy + F_LINK, ws()->link_header);
    ws()->link_header = proxy;
    return NULL;
}

/* The kernel's EnsureFileIsOpen: the data's start and end, reopening a
 * file ResourceFS moved, or one a previous MessageTrans had open. */
static os_error *ensure_open(uint32_t *d, uint32_t *start, uint32_t *end)
{
    uint32_t p = L(*d + F_FILEPTR);
    if (!p) {
        uint32_t flags = L(*d + F_FLAGS);
        if (flags & FLG_ADDTOLIST) {
            uint32_t magic = (flags & FLG_NEW_API) ? MAGIC_FAST
                             : (flags & FLG_WORTH_HASHING) ? 0 : MAGIC_SLOW;
            os_error *e = open_file(*d, L(*d + F_FILENAME), 0, magic);
            if (e)
                return e;
            *d = translate(*d);
        } else if (flags & FLG_INRESOURCEFS) {
            struct info f;
            os_error *e = file_info(L(*d + F_FILENAME), &f);
            if (e)
                return e;
            S(*d + F_FILEPTR, f.data);
        } else {
            return error_lookup(own_block(), ERR_BAD_DESC, "BadDesc", 0);
        }
        p = L(*d + F_FILEPTR);
    }
    *start = p + 4;
    *end = p + L(p);
    return NULL;
}

/* ---- the scanner ---------------------------------------------------------------- */

struct scan {
    uint32_t p, end;                /* the next byte, and after the last */
    uint32_t token;                 /* what is looked for, for the error */
    uint32_t def;                   /* after the token's ":", at EOF */
    os_error *e;
};

/* A token character: LS means it ends the token (the kernel's get_R0_R1). */
static int token_end(uint32_t c)
{
    return c == ',' || c == ')' || c == ':' || c <= ' ';
}

/* The kernel's getbyte: the next byte.  At the end it gives the error,
 * unless the token carries its own default, "token:default". */
static int getbyte(struct scan *s, uint32_t *c)
{
    if (s->p < s->end) {
        *c = B(s->p++);
        return 0;
    }
    uint32_t t = s->token;
    for (uint32_t ch; (ch = B(t)) >= ' '; t++)
        if (ch == ':') {
            s->def = t + 1;
            s->e = ros_error(ERR_TOKEN, "TokNFnd");
            return -1;
        }
    s->def = 0;
    s->e = error_lookup(own_block(), ERR_TOKEN, "TokNFnd", s->token);
    return -1;
}

/* skiptonextline: past the end of this line. */
static int skip_line(struct scan *s, uint32_t c)
{
    for (;;) {
        if (c >= 0x20 || c == 9 || c == 31) {
            if (s->p >= s->end)
                return getbyte(s, &c);
            c = B(s->p++);
            continue;
        }
        if (c == 0x1B) {                /* ESC and its token byte */
            s->p++;
            if (s->p >= s->end)
                return getbyte(s, &c);
            c = B(s->p++);
            continue;
        }
        return s->p >= s->end ? getbyte(s, &c) : 0;
    }
}

/* skiptonexttoken: to the next "/"-alternative, or the next line. */
static int skip_token(struct scan *s, uint32_t c)
{
    for (;;) {
        if (c == '/' || c <= 0x1F)
            return s->p >= s->end ? getbyte(s, &c) : 0;
        if (c == ':')
            return skip_line(s, c);
        if (s->p >= s->end)
            return getbyte(s, &c);
        c = B(s->p++);
    }
}

/* The token at s->token, found: s->p is after its ":"; the value follows.
 * 1 found, 0 not (s->e and maybe s->def set). */
static int find_token(struct scan *s)
{
    uint32_t c8;
    for (;;) {
        uint32_t t = s->token, c0;
        for (;;) {
            c0 = B(t++);
            if (token_end(c0)) {
                if (getbyte(s, &c8))
                    return 0;
                if (c8 == ':' || c8 == '/' || c8 <= 0x1F) {
                    while (c8 != ':')
                        if (getbyte(s, &c8))
                            return 0;
                    return 1;
                }
                break;
            }
            if (getbyte(s, &c8))
                return 0;
            if (c8 == '#') {
                if (skip_line(s, c8))
                    return 0;
                goto next_token;
            }
            if (c8 == ':' || c8 == '/' || c8 <= 0x1F)
                break;
            if (c8 != '?' && c0 != c8)
                break;
        }
        if (skip_token(s, c8))
            return 0;
    next_token:;
    }
}

/* ---- Lookup -------------------------------------------------------------------- */

struct lookup {
    uint32_t token, buffer, size;
    uint32_t params[4];
    uint32_t out_token, out_buffer, out_size;
};

/* The kernel's dictionary token (ESC n), from Resources:$.Resources.Kernel.Dictionary;
 * nothing if there is none. */
static void expand_dictionary(uint32_t n, uint32_t *out, uint32_t *room, uint32_t p0)
{
    uint32_t dict = ws()->dictionary;
    if (!dict) {
        uint32_t f = ros_resourcefs_find("Resources:$.Resources.Kernel.Dictionary");
        if (!f)
            return;
        dict = ws()->dictionary = f + 4;
    }
    uint32_t e;
    if (n == 0) {
        if (!p0)
            return;
        e = p0 - 1;
    } else {
        e = dict;
        for (uint32_t i = 1; i < n; i++) {
            uint32_t len = B(e);
            if (!len)
                return;
            e += len;
        }
        if (!B(e))
            return;
    }
    for (uint32_t c; (c = B(++e)) != 0;) {
        if (c == 0x1B) {
            expand_dictionary(B(++e), out, room, p0);
            if (*room == 0)
                return;
            continue;
        }
        if (--*room <= 0) {
            ros_st8((*out)++, 0);
            *room = 0;
            return;
        }
        ros_st8((*out)++, c);
    }
}

/* The kernel's internal_lookup, in the descriptor d. */
static os_error *internal_lookup(uint32_t d, struct lookup *l)
{
    struct scan s = { 0, 0, l->token, 0, NULL };
    int found = 0;
    os_error *e = ensure_open(&d, &s.p, &s.end);
    if (e) {
        s.p = s.end = 0;                /* as at the end of the file */
        uint32_t c;
        getbyte(&s, &c);
    } else {
        found = find_token(&s);
    }
    uint32_t value, t;
    if (found) {
        value = s.p;
        t = l->token;
        while (!token_end(B(t)))
            t++;
    } else {
        if (!s.def)
            return s.e;
        value = s.def;                  /* the token's default, to its end */
        uint32_t q = value;
        while (B(q) != 10 && B(q) != 13 && B(q) != 0)
            q++;
        t = q;
        s.p = value;
        s.end = q + 1;
    }
    l->out_token = t;

    uint32_t c;
    if (!l->buffer) {                   /* a pointer into the file */
        l->out_buffer = value;
        do {
            if (getbyte(&s, &c))
                return s.e;
            if (c == 0x1B)
                s.p++;
        } while (c != 10 && c != 13 && c != 0);
        l->out_size = s.p - 1 - value;
        return NULL;
    }
    uint32_t out = l->buffer, room = l->size;
    for (;;) {
        if (getbyte(&s, &c))
            return s.e;
        if (c == '%') {
            if (getbyte(&s, &c))
                return s.e;
            uint32_t k = c - '0';
            if (k < 4 && l->params[k]) {
                uint32_t p = l->params[k];
                for (uint32_t ch; (ch = B(p)) != 0 && ch != 10 && ch != 13; p++) {
                    if (--room == 0)
                        break;
                    ros_st8(out++, ch);
                }
                if (room == 0) {
                    ros_st8(out++, 0);
                    break;
                }
                continue;
            }
            if (c != '%')
                s.p--;                  /* "%x" stays "%x" */
            c = '%';
        } else if (c == 0x1B) {
            uint32_t n;
            if (getbyte(&s, &n))
                return s.e;
            expand_dictionary(n, &out, &room, l->params[0]);
            if ((int32_t)room > 0)
                continue;
            break;
        }
        if (c == 10 || c == 0 || c == 13 || --room == 0) {
            ros_st8(out++, 0);
            break;
        }
        ros_st8(out++, c);
    }
    l->out_buffer = l->buffer;
    l->out_size = out - l->buffer - 1;
    return NULL;
}

/* MessageTrans_Lookup: the descriptor's file, then the Global messages. */
static os_error *lookup(uint32_t desc, struct lookup *l)
{
    uint32_t d = translate(desc);
    if (d && !internal_lookup(d, l))
        return NULL;
    uint32_t g = translate(ws()->global[0]);
    if (!g)
        return ros_error(ERR_TOKEN, "TokNFnd");
    return internal_lookup(g, l);
}

/* MessageTrans_ErrorLookup, for our own errors and the Global ones:
 * errnum and token, %0 p0, into an internal buffer. */
static os_error *error_lookup(uint32_t block, uint32_t errnum, const char *token, uint32_t p0)
{
    static unsigned depth;              /* no "TokNFnd" anywhere: not forever */
    if (depth)
        return ros_error(errnum, "%s", token);
    uint32_t buf = internal_buffer();
    uint32_t tok = buf + BUFFER_SIZE - 24;          /* the token, at the buffer's end */
    strcpy(ros_ptr(tok), token);
    struct lookup l = { tok, buf + 4, BUFFER_SIZE - 28, { p0, 0, 0, 0 }, 0, 0, 0 };
    depth++;
    os_error *e = lookup(block, &l);
    depth--;
    if (e)
        return e;
    S(buf, errnum);
    return ros_ptr(buf);
}

/* ---- the SWIs ------------------------------------------------------------------ */

#define ENTER(s)                                                            \
    uint32_t outer_sp_ = ros_svc_sp;                                        \
    ros_svc_sp = (s)->r[13];                                                \
    (s)->v = 0
#define LEAVE(s, e)                                                         \
    do {                                                                    \
        if (e)                                                              \
            ros_swi_fail((s), (e));                                         \
        ros_svc_sp = outer_sp_;                                             \
    } while (0)

void ros_thunk_MessageTrans_FileInfo(struct ros_cpu *s)
{
    ENTER(s);
    struct info f;
    os_error *e = file_info(s->r[1], &f);
    if (!e) {
        s->r[0] = f.flags & ~FLG_HIDDEN;
        s->r[2] = f.size;
    }
    LEAVE(s, e);
}

void ros_thunk_MessageTrans_OpenFile(struct ros_cpu *s)
{
    ENTER(s);
    os_error *e = open_file(s->r[0], s->r[1], s->r[2], s->r[3]);
    LEAVE(s, e);
}

void ros_thunk_MessageTrans_Lookup(struct ros_cpu *s)
{
    ENTER(s);
    struct lookup l = { s->r[1], s->r[2], s->r[3], { s->r[4], s->r[5], s->r[6], s->r[7] },
                        0, 0, 0 };
    os_error *e = lookup(s->r[0], &l);
    if (!e) {
        s->r[1] = l.out_token;
        s->r[2] = l.out_buffer;
        s->r[3] = l.out_size;
    }
    LEAVE(s, e);
}

void ros_thunk_MessageTrans_GSLookup(struct ros_cpu *s)
{
    if (!s->r[2]) {
        ros_thunk_MessageTrans_Lookup(s);
        return;
    }
    ENTER(s);
    os_error *e = NULL;
    uint8_t *tmp = ros_rma_alloc(s->r[3] ? s->r[3] : 1);
    if (!tmp) {
        e = ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    } else {
        struct lookup l = { s->r[1], ros_addr(tmp), s->r[3],
                            { s->r[4], s->r[5], s->r[6], s->r[7] }, 0, 0, 0 };
        e = lookup(s->r[0], &l);
        if (!e) {
            struct ros_cpu g;
            ros_cpu_enter(&g);
            g.r[0] = l.out_buffer, g.r[1] = s->r[2], g.r[2] = s->r[3];
            ros_swi(&g, XOS_GSTrans);
            if (g.v) {
                e = ros_ptr(g.r[0]);
            } else {
                s->r[3] = g.r[2];       /* R2 stays the caller's buffer */
            }
        }
        ros_rma_free(tmp);
    }
    LEAVE(s, e);
}

void ros_thunk_MessageTrans_ErrorLookup(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t buf = s->r[2], size = s->r[3];
    if (!buf) {
        buf = internal_buffer();
        size = BUFFER_SIZE;
    }
    uint32_t errnum = L(s->r[0]);
    struct lookup l = { s->r[0] + 4, buf + 4, size - 4, { s->r[4], s->r[5], s->r[6], s->r[7] },
                        0, 0, 0 };
    os_error *e = lookup(s->r[1], &l);
    if (!e) {
        S(buf, errnum);
        e = ros_ptr(buf);
    }
    LEAVE(s, e);                        /* always V set: the error, or the translation */
}

void ros_thunk_MessageTrans_CopyError(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t buf = internal_buffer(), from = s->r[0];
    S(buf, L(from));
    uint32_t n = 0;
    for (from += 4; n < BUFFER_SIZE - 5; n++) {
        uint32_t c = B(from + n);
        ros_st8(buf + 4 + n, c);
        if (!c)
            break;
    }
    if (n == BUFFER_SIZE - 5)
        ros_st8(buf + 4 + n, 0);
    LEAVE(s, (os_error *)ros_ptr(buf));
}

void ros_thunk_MessageTrans_CloseFile(struct ros_cpu *s)
{
    ENTER(s);
    os_error *e = close_file(s->r[0]);
    LEAVE(s, e);
}

/* MessageTrans_EnumerateTokens: the tokens matching R1 ("?" any one
 * character, a final "*" any rest), from place R4. */
void ros_thunk_MessageTrans_EnumerateTokens(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t d = translate(s->r[0]), pattern = s->r[1];
    struct scan sc = { 0, 0, pattern, 0, NULL };
    os_error *e = ensure_open(&d, &sc.p, &sc.end);
    if (e) {
        LEAVE(s, e);
        return;
    }
    uint32_t data = sc.p;
    sc.p = data + s->r[4];
    uint32_t c8, c0;
    if (sc.p >= sc.end)
        goto finished;
    for (;;) {
        uint32_t t = pattern, start = sc.p;
        for (;;) {
            c0 = B(t++);
            if (token_end(c0)) {
                if (getbyte(&sc, &c8))
                    goto finished;
                if (c8 == '/' || c8 == ':' || c8 <= 0x1F) {
                    while (c8 != ':')
                        if (getbyte(&sc, &c8))
                            goto finished;
                    goto found;
                }
                break;
            }
            if (getbyte(&sc, &c8))
                goto finished;
            if (c8 == '#') {
                if (skip_line(&sc, c8))
                    goto finished;
                goto next;
            }
            if (c0 == '*') {
                if (token_end(B(t)))
                    goto found;
                e = error_lookup(own_block(), ERR_SYNTAX, "Syntax", pattern);
                LEAVE(s, e);
                return;
            }
            if (c8 == '/' || c8 == ':' || c8 <= 0x1F)
                break;
            if (c8 != '?' && c0 != c8 && c0 != '?')
                break;
        }
        if (skip_token(&sc, c8))
            goto finished;
        continue;
    found: {
            uint32_t term = token_end(c0) ? c0 : B(t), out = s->r[2];
            int32_t room = (int32_t)s->r[3];
            sc.p = start;
            for (;;) {
                if (getbyte(&sc, &c8)) {
                    LEAVE(s, sc.e);
                    return;
                }
                if (c8 == '/' || c8 == ':' || c8 <= 0x1F)
                    break;
                if (--room < 0) {
                    e = error_lookup(0, ERR_BUFF_OVERFLOW, "BufOFlo", 0);
                    LEAVE(s, e);
                    return;
                }
                ros_st8(out++, c8);
            }
            if (--room < 0) {
                e = error_lookup(0, ERR_BUFF_OVERFLOW, "BufOFlo", 0);
                LEAVE(s, e);
                return;
            }
            ros_st8(out, term);
            uint32_t len = out - s->r[2];
            skip_token(&sc, c8);        /* where the next call starts */
            if (!len)
                continue;               /* a null token: on */
            s->r[4] = sc.p - data;
            s->r[3] = len;
            s->v = 0;
            ros_svc_sp = outer_sp_;
            return;
        }
    next:;
    }
finished:
    s->r[2] = 0;
    s->v = 0;
    ros_svc_sp = outer_sp_;
}

/* MessageTrans_MakeMenus: Wimp menus from a definition of tokens. */
void ros_thunk_MessageTrans_MakeMenus(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t desc = s->r[0], def = s->r[1], buf = s->r[2];
    int32_t room = (int32_t)s->r[3];
    os_error *e = NULL;
    while (B(def)) {
        if (room < 28) {
            e = error_lookup(0, ERR_BUFF_OVERFLOW, "BufOFlo", 0);
            break;
        }
        uint32_t menu = buf;
        struct lookup l = { def, buf, (uint32_t)room, { 0, 0, 0, 0 }, 0, 0, 0 };
        if ((e = lookup(desc, &l)))
            break;
        int32_t widest = (int32_t)l.out_size - 3;
        def = l.out_token + 1;
        for (int i = 0; i < 4; i++)
            ros_st8(menu + 12 + (uint32_t)i, B(def++));     /* colours */
        S(menu + 20, B(def++));                             /* item height */
        S(menu + 24, B(def++));                             /* gap */
        buf = menu + 28;
        room -= 28;
        for (;;) {
            if (room < 24) {
                e = error_lookup(0, ERR_BUFF_OVERFLOW, "BufOFlo", 0);
                goto out;
            }
            uint32_t t = def;
            while (B(t) > 32 && B(t) != ',' && B(t) != ')')
                t++;
            uint32_t words = (t + 1 + 3) & ~3u;
            uint32_t item_flags = L(words), sub = L(words + 4), icon_flags = L(words + 8);
            uint32_t tbuf, tsize;
            if ((item_flags & 4) && (icon_flags & 0x100)) {     /* writable, indirected */
                tbuf = L(buf + 12);
                tsize = L(buf + 20);
            } else if (!(icon_flags & 0x100)) {
                tbuf = buf + 12;
                tsize = 12;
            } else {
                tbuf = 0;
                tsize = 0;
            }
            struct lookup li = { def, tbuf, tsize, { 0, 0, 0, 0 }, 0, 0, 0 };
            if ((e = lookup(desc, &li)))
                goto out;
            def = words + 12;
            S(buf, item_flags);
            S(buf + 4, sub ? menu + sub : 0);
            S(buf + 8, icon_flags);
            if ((icon_flags & 0x100) && !(item_flags & 4)) {
                S(buf + 12, li.out_buffer);
                S(buf + 16, 0);
                S(buf + 20, li.out_size + 1);
            }
            buf += 24;                  /* room is not reduced: the kernel's does not */
            if ((int32_t)li.out_size > widest)
                widest = (int32_t)li.out_size;
            if (item_flags & 0x80)
                break;
        }
        S(menu + 16, (uint32_t)widest * 16 + 12);
    }
out:
    if (!e) {
        s->r[2] = buf;
        s->r[3] = (uint32_t)room;
    }
    LEAVE(s, e);
}

void ros_thunk_MessageTrans_Dictionary(struct ros_cpu *s)
{
    ENTER(s);
    os_error *e = NULL;
    if (!ws()->dictionary) {
        uint32_t f = ros_resourcefs_find("Resources:$.Resources.Kernel.Dictionary");
        if (f)
            ws()->dictionary = f + 4;
        else
            e = error_lookup(0, 0xD6, "NoFile", 0);
    }
    if (!e)
        s->r[0] = ws()->dictionary;
    LEAVE(s, e);
}

/* ---- the module ---------------------------------------------------------------------- */

static const char own_name[] = "Resources:$.Resources.MsgTrans.Messages";
static const char global_name[] = "Resources:$.Resources.Global.Messages";

/* The kernel's AttemptOpenMessagesFiles: ours and the Global, quietly. */
static void open_own_files(void)
{
    struct workspace *w = ws();
    uint32_t names = ros_ld32(messagetrans_module.private_word) + sizeof *w;
    if (!w->own[0]) {
        uint32_t d = arena_of(&w->own[1]);
        if (!open_file(d, names, 0, 0))
            w->own[0] = d;
    }
    if (!w->global[0]) {
        uint32_t d = arena_of(&w->global[1]);
        if (!open_file(d, names + sizeof own_name, 0, 0))
            w->global[0] = d;
    }
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    uint8_t *block = ros_rma_alloc(sizeof(struct workspace) + sizeof own_name + sizeof global_name);
    if (!block)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(block, 0, sizeof(struct workspace));
    memcpy(block + sizeof(struct workspace), own_name, sizeof own_name);
    memcpy(block + sizeof(struct workspace) + sizeof own_name, global_name, sizeof global_name);
    ros_st32(m->private_word, ros_addr(block));
    open_own_files();
    return NULL;
}

/* The kernel's Die: every file freed; each client's block marked, so a
 * MessageTrans started later reopens it. */
static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    while (w->link_header) {
        uint32_t d = w->link_header, client = L(d + F_CLNT_BLOCK);
        w->link_header = L(d + F_LINK);
        free_data(d);
        if (client && L(client) == MAGIC_FAST) {
            S(client + F_FILEPTR, 0);
            S(client + 0, 0);
            uint32_t flags = L(client + F_FLAGS) & (FLG_WORTH_HASHING | FLG_NEW_API);
            S(client + F_FLAGS, flags | FLG_ADDTOLIST);
        }
    }
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

/* ResourceFS changed: files that lay in it, and moved, are looked for
 * again when next used; clients are told (Service_MessageFileClosed). */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    uint32_t n = s->r[1];
    if (n == SERVICE_MODULE_POST_INIT) {
        if (s->r[0] == m->base) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = 0, c.r[1] = SERVICE_MESSAGE_FILE_CLOSED;
            ros_service_call(&c);
        }
        return;
    }
    if (n != SERVICE_RESOURCEFS_STARTED && n != SERVICE_RESOURCEFS_DYING &&
        n != SERVICE_TERRITORY_STARTED)
        return;
    int changed = 0;
    for (uint32_t d = ws()->link_header; d; d = L(d + F_LINK)) {
        if (!(L(d + F_FLAGS) & FLG_INRESOURCEFS))
            continue;
        struct info f;
        if (!file_info(L(d + F_FILENAME), &f) && f.flags == FLG_INRESOURCEFS &&
            f.data == L(d + F_FILEPTR))
            continue;                   /* it has not moved */
        S(d + F_FILEPTR, 0);
        changed = 1;
    }
    if (changed && n != SERVICE_RESOURCEFS_DYING) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0, c.r[1] = SERVICE_MESSAGE_FILE_CLOSED;
        ros_service_call(&c);
    }
    if (n == SERVICE_RESOURCEFS_STARTED)
        open_own_files();
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    static const char title[] = "MessageTrans";
    uint32_t t = internal_buffer();
    strcpy(ros_ptr(t + 200), title);
    return error_lookup(own_block(), ROS_ERR_NO_SUCH_SWI, "BadSWI", t + 200);
}

struct ros_module messagetrans_module = {
    .title = "MessageTrans",
    .help = "MessageTrans\t0.49 (25 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x41500,
    .swi_thunks = ros_swi_thunks_MessageTrans,
    .swi_names = ros_swi_names_MessageTrans,
    .swi_prefix = "MessageTrans",
};

__attribute__((constructor)) static void count(void)
{
    messagetrans_module.swi_count = ros_swi_count_MessageTrans;
}
