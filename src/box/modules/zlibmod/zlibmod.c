/* Copyright RISC OS Open Ltd and others
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
 * This file is derived from RISC OS Open's source and from other code.
 * The Apache licence of RISC OS Open's source applies to it.
 */

/* zlibmod.c: ZLib (Programmer/ZLibMod, 0.05, 28 May 2022) as a native
 * module over zlib (deps/build-imagelibs.sh). It provides the module's 38
 * SWIs, register for register as ZLibMod's c/cmodule makes them, and the
 * task association of its c/task. README.md lists the SWIs and the
 * differences from RISC OS 5.30.
 *
 * A RISC OS program's z_stream is 56 bytes of its own memory (hdr/ZLib).
 * The host's z_stream is twice that size. zlib's internal state also
 * points back at the z_stream it belongs to. So the program's block cannot
 * be the host's z_stream.
 * Each stream is instead a record in the RMA. The record holds the host
 * z_stream and the owning task. The block's "state" word names the
 * record. RISC OS programs only pass that word back. Before each call, the
 * block's public words (next_in, avail_in, totals, next_out, avail_out,
 * data_type, adler) are copied into the host's stream. After the call, all
 * of them are copied back out, and so are msg and state. What the program
 * reads afterwards is what zlib left, as on RISC OS.
 * zlib allocates its state with the module's allocator, in the RMA, so the
 * whole of a stream is arena memory.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zlib.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "zlibmod.h"

#define REC_MAGIC 0x52545354u           /* "TSTR" */

/* A stream: the host's z_stream, and which task it belongs to (c/task's
 * task_stream, kept with the stream) */
struct zrec {
    uint32_t magic;
    uint32_t scb;                       /* the program's block */
    uint32_t next;                      /* the next record, or 0 */
    uint32_t task;                      /* its task, 0 if none */
    int isdeflate;
    int active;                         /* c/task's isactive: ended at the task's end */
    int gzip;                           /* deflating with a gzip wrapper: gzhead in use */
    gz_header gzhead;                   /* ... RISC OS's header: OS 13 (zutil's OS_CODE there) */
    z_stream zs;
};

/* A gzip wrapper's header as RISC OS's zlib writes it by default: the
 * default header's fields (no name, comment, extra or time), its operating
 * system RISC OS's, 13, where the host's zlib would write its own.  Kept
 * through resets, as the OS code is there. */
static void riscos_header(struct zrec *z)
{
    memset(&z->gzhead, 0, sizeof z->gzhead);
    z->gzhead.os = 13;
    if (deflateSetHeader(&z->zs, &z->gzhead) == Z_OK)
        z->gzip = 1;
}

/* ---- the workspace, errors, memory ---------------------------------------------- */

struct zm_workspace *zm_ws(void)
{
    return ros_ptr(ros_ld32(zlib_module.private_word));
}

/* Resources.ZLib.Messages's texts, by token (c/errors) */
os_error *zm_error(int which)
{
    static const char *const text[] = {
        "Insufficient memory for ZLib operation", "Invalid ZLib_Compress flag",
        "Invalid ZLib_Decompress flag", "ZLib error", "Unknown ZLib SWI",
        "Invalid ZLib_TaskAssociate operation", "Invalid ZLib_GZSeek parameter",
    };
    return ros_error(ZM_ERRBASE + (uint32_t)which, "%s", text[which]);
}

os_error *zm_zlib_error(int err, const char *msg)
{
    char num[32];
    const char *e;
    switch (err) {
    case Z_ERRNO: e = "Z_ERRNO 0"; break;
    case Z_STREAM_ERROR: e = "Z_STREAM_ERROR"; break;
    case Z_DATA_ERROR: e = "Z_DATA_ERROR"; break;
    case Z_MEM_ERROR: e = "Z_MEM_ERROR"; break;
    case Z_BUF_ERROR: e = "Z_BUF_ERROR"; break;
    case Z_VERSION_ERROR: e = "Z_VERSION_ERROR"; break;
    default: snprintf(num, sizeof num, "%d", err); e = num; break;
    }
    if (msg)
        return ros_error(ZM_ERRBASE + ZM_E_ZLIBERR, "ZLib error %s: %s", e, msg);
    return ros_error(ZM_ERRBASE + ZM_E_ZLIBERR, "ZLib error %s", e);
}

os_error *zm_abort(uint32_t addr)
{
    return ros_error(0x80000002u, "Internal error: abort on data transfer at &%08X", addr);
}

int zm_valid(uint32_t a, uint32_t len)
{
    if (len == 0)
        return 1;
    if (a == 0 || a + len < a)
        return 0;
    int invalid = 1;
    xos_validate_address(a, a + len, &invalid);
    return !invalid;
}

void *zm_zalloc(void *opaque, unsigned items, unsigned size)
{
    (void)opaque;
    uint64_t n = (uint64_t)items * size;
    void *b;
    if (n == 0 || n > 0x7FFFFFF0u || xos_module_claim((uint32_t)n, &b))
        return NULL;
    return b;
}

void zm_zfree(void *opaque, void *p)
{
    (void)opaque;
    if (p)
        xos_module_free(p);
}

uint32_t zm_string(const char *s)
{
    if (!s)
        return 0;
    if (ros_in_arena(s))
        return ros_addr(s);
    struct zm_workspace *w = zm_ws();
    for (uint32_t i = 0; i < w->strings_used; i += (uint32_t)strlen(w->strings + i) + 1)
        if (!strcmp(w->strings + i, s))
            return ros_addr(w->strings + i);
    size_t n = strlen(s) + 1;
    if (w->strings_used + n > sizeof w->strings)
        return ros_addr(w->strings + w->strings_used - 1);     /* full: "" */
    memcpy(w->strings + w->strings_used, s, n);
    w->strings_used += (uint32_t)n;
    return ros_addr(w->strings + w->strings_used - n);
}

os_error *zm_swi(uint32_t number, uint32_t *r)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 8 * 4);
    ros_swi(&c, number);
    memcpy(r, c.r, 8 * 4);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* The current Wimp task (Wimp_ReadSysInfo 5), or 0 (c/task's
 * get_task_handle) */
static uint32_t current_task(void)
{
    uint32_t r[8] = { 5 };
    return zm_swi(XWimp_ReadSysInfo, r) ? 0 : r[0];
}

/* ---- streams ------------------------------------------------------------------------- */

static void *ptr_or_null(uint32_t a)
{
    return a ? ros_ptr(a) : NULL;
}

static uint32_t addr_or_0(const void *p)
{
    return p ? ros_addr(p) : 0;
}

/* The record a block's state word names, if it is a live one of this
 * block's (zlib's own deflateStateCheck, in effect) */
static struct zrec *rec_of(uint32_t scb)
{
    uint32_t a = ros_ld32(scb + ZM_STATE);
    if (!zm_valid(a, sizeof(struct zrec)) || (a & 3))
        return NULL;
    struct zrec *z = ros_ptr(a);
    return z->magic == REC_MAGIC && z->scb == scb ? z : NULL;
}

/* The block's public words into the host's stream */
static void sync_in(struct zrec *z, uint32_t scb)
{
    z_stream *s = &z->zs;
    s->next_in = ptr_or_null(ros_ld32(scb + ZM_NEXT_IN));
    s->avail_in = ros_ld32(scb + ZM_AVAIL_IN);
    s->total_in = ros_ld32(scb + ZM_TOTAL_IN);
    s->next_out = ptr_or_null(ros_ld32(scb + ZM_NEXT_OUT));
    s->avail_out = ros_ld32(scb + ZM_AVAIL_OUT);
    s->total_out = ros_ld32(scb + ZM_TOTAL_OUT);
    if (ros_ld32(scb + ZM_MSG) == 0)
        s->msg = NULL;
    s->data_type = (int)ros_ld32(scb + ZM_DATA_TYPE);
    s->adler = ros_ld32(scb + ZM_ADLER);
}

/* ... and all of it back, the state word last: the record, or 0 once zlib
 * has let the stream go */
static void sync_out(struct zrec *z, uint32_t scb)
{
    z_stream *s = &z->zs;
    ros_st32(scb + ZM_NEXT_IN, addr_or_0(s->next_in));
    ros_st32(scb + ZM_AVAIL_IN, s->avail_in);
    ros_st32(scb + ZM_TOTAL_IN, (uint32_t)s->total_in);
    ros_st32(scb + ZM_NEXT_OUT, addr_or_0(s->next_out));
    ros_st32(scb + ZM_AVAIL_OUT, s->avail_out);
    ros_st32(scb + ZM_TOTAL_OUT, (uint32_t)s->total_out);
    ros_st32(scb + ZM_MSG, zm_string(s->msg));
    ros_st32(scb + ZM_DATA_TYPE, (uint32_t)s->data_type);
    ros_st32(scb + ZM_ADLER, (uint32_t)s->adler);
    ros_st32(scb + ZM_STATE, s->state ? ros_addr(z) : 0);
}

static void unlink_rec(struct zrec *z)
{
    struct zm_workspace *w = zm_ws();
    uint32_t a = ros_addr(z), *p = &w->streams;
    while (*p && *p != a)
        p = &((struct zrec *)ros_ptr(*p))->next;
    if (*p)
        *p = z->next;
}

static void free_rec(struct zrec *z)
{
    unlink_rec(z);
    z->magic = 0;
    xos_module_free(z);
}

/* After a call: a stream zlib has let go of is forgotten */
static void settle(struct zrec *z, uint32_t scb)
{
    sync_out(z, scb);
    if (!z->zs.state)
        free_rec(z);
}

/* c/task's task_strmcreate: the stream belongs to the current task, if any */
static void associate(struct zrec *z, int isdeflate)
{
    z->isdeflate = isdeflate;
    uint32_t t = current_task();
    if (t) {
        z->task = t;
        z->active = 1;
    }
}

/* A new record for a block, linked in. NULL if the RMA is full. If the
 * block already had a record (it was initialised twice without an end),
 * that record is ended and freed. RISC OS would leak its memory. */
static struct zrec *new_rec(uint32_t scb)
{
    struct zrec *old = rec_of(scb);
    if (old) {
        if (old->isdeflate)
            deflateEnd(&old->zs);
        else
            inflateEnd(&old->zs);
        free_rec(old);
    }
    struct zrec *z;
    if (xos_module_claim(sizeof *z, (void **)&z))
        return NULL;
    memset(z, 0, sizeof *z);
    z->magic = REC_MAGIC;
    z->scb = scb;
    z->zs.zalloc = zm_zalloc;
    z->zs.zfree = zm_zfree;
    struct zm_workspace *w = zm_ws();
    z->next = w->streams;
    w->streams = ros_addr(z);
    return z;
}

/* What an init's version and size arguments say, checked as zlib checks
 * them, against RISC OS's z_stream; 0 if they will do */
static int version_bad(uint32_t version, uint32_t size)
{
    return version == 0 || !zm_valid(version, 1) || ros_ld8(version) != ZLIB_VERSION[0] ||
           size != ZM_SCB_SIZE;
}

/* The buffers a call will read and write, as the block gives them */
static os_error *buffers_ok(uint32_t scb)
{
    uint32_t in = ros_ld32(scb + ZM_NEXT_IN), out = ros_ld32(scb + ZM_NEXT_OUT);
    if (!zm_valid(in, ros_ld32(scb + ZM_AVAIL_IN)))
        return zm_abort(in);
    if (!zm_valid(out, ros_ld32(scb + ZM_AVAIL_OUT)))
        return zm_abort(out);
    return NULL;
}

enum init_kind { DEFLATE_INIT, INFLATE_INIT, DEFLATE_INIT2, INFLATE_INIT2 };

/* DeflateInit, InflateInit, DeflateInit2, InflateInit2: R0 the code */
static os_error *init_stream(uint32_t *r, enum init_kind kind)
{
    uint32_t scb = r[0], version, size;
    switch (kind) {
    case DEFLATE_INIT: version = r[2]; size = r[3]; break;
    case INFLATE_INIT: version = r[1]; size = r[2]; break;
    case DEFLATE_INIT2: version = r[6]; size = r[7]; break;
    default: version = r[2]; size = r[3]; break;
    }
    if (version_bad(version, size)) {
        r[0] = (uint32_t)Z_VERSION_ERROR;
        return NULL;
    }
    if (scb == 0) {
        r[0] = (uint32_t)Z_STREAM_ERROR;
        return NULL;
    }
    if (!zm_valid(scb, ZM_SCB_SIZE))
        return zm_abort(scb);
    ros_st32(scb + ZM_MSG, 0);
    struct zrec *z = new_rec(scb);
    if (!z) {
        r[0] = (uint32_t)Z_MEM_ERROR;
        return NULL;
    }
    sync_in(z, scb);
    int ret;
    switch (kind) {
    case DEFLATE_INIT: ret = deflateInit(&z->zs, (int)r[1]); break;
    case INFLATE_INIT: ret = inflateInit(&z->zs); break;
    case DEFLATE_INIT2:
        ret = deflateInit2(&z->zs, (int)r[1], (int)r[2], (int)r[3], (int)r[4], (int)r[5]);
        break;
    default: ret = inflateInit2(&z->zs, (int)r[1]); break;
    }
    if (ret == Z_OK) {
        associate(z, kind == DEFLATE_INIT || kind == DEFLATE_INIT2);
        if (kind == DEFLATE_INIT2 && (int32_t)r[3] > 15)
            riscos_header(z);
        sync_out(z, scb);
    } else {
        /* zlib touched only msg; the record goes */
        ros_st32(scb + ZM_MSG, zm_string(z->zs.msg));
        free_rec(z);
    }
    r[0] = (uint32_t)ret;
    return NULL;
}

/* A call on a live stream: R0 the code. op is the zlib call; buffers says
 * whether it moves data through next_in and next_out. */
enum stream_op {
    OP_DEFLATE, OP_DEFLATE_END, OP_INFLATE, OP_INFLATE_END, OP_DEFLATE_SET_DICT,
    OP_DEFLATE_RESET, OP_DEFLATE_PARAMS, OP_INFLATE_SET_DICT, OP_INFLATE_SYNC,
    OP_INFLATE_RESET, OP_INFLATE_RESET2, OP_DEFLATE_PENDING, OP_DEFLATE_GET_DICT,
    OP_INFLATE_GET_DICT,
};

static os_error *stream_call(uint32_t *r, enum stream_op op)
{
    uint32_t scb = r[0];
    if (scb == 0) {
        r[0] = (uint32_t)Z_STREAM_ERROR;
        return NULL;
    }
    if (!zm_valid(scb, ZM_SCB_SIZE))
        return zm_abort(scb);
    struct zrec *z = rec_of(scb);
    if (!z) {
        r[0] = (uint32_t)Z_STREAM_ERROR;
        return NULL;
    }
    os_error *e;
    if ((op == OP_DEFLATE || op == OP_INFLATE || op == OP_INFLATE_SYNC || op == OP_DEFLATE_PARAMS) &&
        (e = buffers_ok(scb)))
        return e;
    sync_in(z, scb);
    z_stream *s = &z->zs;
    int ret;
    switch (op) {
    case OP_DEFLATE: ret = deflate(s, (int)r[1]); break;
    case OP_INFLATE: ret = inflate(s, (int)r[1]); break;
    case OP_DEFLATE_END: ret = deflateEnd(s); break;
    case OP_INFLATE_END: ret = inflateEnd(s); break;
    case OP_DEFLATE_RESET: ret = deflateReset(s); break;
    case OP_INFLATE_RESET: ret = inflateReset(s); break;
    case OP_INFLATE_RESET2: ret = inflateReset2(s, (int)r[1]); break;
    case OP_INFLATE_SYNC: ret = inflateSync(s); break;
    case OP_DEFLATE_PARAMS: ret = deflateParams(s, (int)r[1], (int)r[2]); break;
    case OP_DEFLATE_SET_DICT:
    case OP_INFLATE_SET_DICT:
        if (!zm_valid(r[1], r[2]))
            return zm_abort(r[1]);
        ret = op == OP_DEFLATE_SET_DICT ? deflateSetDictionary(s, ptr_or_null(r[1]), r[2])
                                        : inflateSetDictionary(s, ptr_or_null(r[1]), r[2]);
        break;
    case OP_DEFLATE_PENDING: {
        unsigned pending = 0;
        int bits = 0;
        if (!zm_valid(r[1], r[1] ? 4 : 0) || !zm_valid(r[2], r[2] ? 4 : 0))
            return zm_abort(r[1] ? r[1] : r[2]);
        ret = deflatePending(s, r[1] ? &pending : NULL, r[2] ? &bits : NULL);
        if (ret == Z_OK && r[1])
            ros_st32(r[1], pending);
        if (ret == Z_OK && r[2])
            ros_st32(r[2], (uint32_t)bits);
        break;
    }
    default: {                          /* the GetDictionary pair */
        uInt len = r[2] && zm_valid(r[2], 4) ? ros_ld32(r[2]) : 0;
        if (r[2] && !zm_valid(r[2], 4))
            return zm_abort(r[2]);
        /* the dictionary is at most the window: 32K */
        if (r[1] && !zm_valid(r[1], 32768u))
            return zm_abort(r[1]);
        ret = op == OP_DEFLATE_GET_DICT ? deflateGetDictionary(s, ptr_or_null(r[1]), r[2] ? &len : NULL)
                                        : inflateGetDictionary(s, ptr_or_null(r[1]), r[2] ? &len : NULL);
        if (r[2])
            ros_st32(r[2], len);
        break;
    }
    }
    settle(z, scb);
    r[0] = (uint32_t)ret;
    return NULL;
}

/* DeflateCopy: R0 -> destination, R1 -> source. The source's whole block
 * is copied, as zlib copies the z_stream. */
static os_error *deflate_copy(uint32_t *r)
{
    uint32_t dst = r[0], src = r[1];
    if (src == 0 || dst == 0) {
        r[0] = (uint32_t)Z_STREAM_ERROR;
        return NULL;
    }
    if (!zm_valid(src, ZM_SCB_SIZE))
        return zm_abort(src);
    if (!zm_valid(dst, ZM_SCB_SIZE))
        return zm_abort(dst);
    struct zrec *from = rec_of(src);
    if (!from || !from->isdeflate || dst == src) {      /* new_rec would free the source */
        r[0] = (uint32_t)Z_STREAM_ERROR;
        return NULL;
    }
    struct zrec *to = new_rec(dst);
    if (!to) {
        r[0] = (uint32_t)Z_MEM_ERROR;
        return NULL;
    }
    sync_in(from, src);
    int ret = deflateCopy(&to->zs, &from->zs);
    if (ret == Z_OK) {
        if (from->gzip)                 /* its own header, not the source's */
            riscos_header(to);
        memmove(ros_ptr(dst), ros_ptr(src), ZM_SCB_SIZE);
        associate(to, 1);
        sync_out(to, dst);
    } else {
        free_rec(to);
    }
    r[0] = (uint32_t)ret;
    return NULL;
}

/* ---- ZLib_Compress and ZLib_Decompress: Squash's interface ------------------------- */

#define COMPRESS_CONTINUE   0x01u
#define COMPRESS_MORE       0x02u
#define DECOMPRESS_WILLFIT  0x04u
#define RETURN_WORKSPACE    0x08u
#define NOT_BOUND           0x10u
#define COMPRESS_VALID      0x1Bu
#define DECOMPRESS_VALID    0x1Fu

/* compressBound, in RISC OS's 32 bits */
static uint32_t bound32(uint32_t n)
{
    return n + (n >> 12) + (n >> 14) + (n >> 25) + 13u;
}

static os_error *squash_like(uint32_t *r, int compress)
{
    uint32_t flags = r[0];
    if (flags & ~(compress ? COMPRESS_VALID : DECOMPRESS_VALID))
        return zm_error(compress ? ZM_E_INVCFLG : ZM_E_INVDFLG);
    if (flags & RETURN_WORKSPACE) {
        r[0] = ZM_SCB_SIZE;
        if (!compress)
            r[1] = 0xFFFFFFFFu;
        else if (r[1] != 0xFFFFFFFFu)
            r[1] = bound32(r[1]);
        return NULL;
    }
    uint32_t scb = r[1];
    if (!zm_valid(scb, ZM_SCB_SIZE))
        return zm_abort(scb);
    struct zrec *z;
    if (!(flags & COMPRESS_CONTINUE)) {
        ros_st32(scb + ZM_ZALLOC, 0);
        ros_st32(scb + ZM_ZFREE, 0);
        ros_st32(scb + ZM_OPAQUE, 0);
        ros_st32(scb + ZM_MSG, 0);
        z = new_rec(scb);
        if (!z)
            return zm_zlib_error(Z_MEM_ERROR, NULL);
        sync_in(z, scb);
        int ret = compress ? deflateInit(&z->zs, 9) : inflateInit(&z->zs);
        if (ret != Z_OK) {
            os_error *e = zm_zlib_error(ret, z->zs.msg);
            free_rec(z);
            return e;
        }
        z->isdeflate = compress;
        if (!(flags & NOT_BOUND))
            associate(z, compress);
        sync_out(z, scb);
    } else if (!(z = rec_of(scb))) {
        return zm_zlib_error(Z_STREAM_ERROR, NULL);
    }
    ros_st32(scb + ZM_NEXT_IN, r[2]);
    ros_st32(scb + ZM_AVAIL_IN, r[3]);
    ros_st32(scb + ZM_NEXT_OUT, r[4]);
    ros_st32(scb + ZM_AVAIL_OUT, r[5]);
    os_error *e = buffers_ok(scb);
    if (e)
        return e;
    sync_in(z, scb);
    int more = (flags & COMPRESS_MORE) != 0;
    int ret = compress ? deflate(&z->zs, more ? Z_NO_FLUSH : Z_FINISH)
                       : inflate(&z->zs, more ? Z_NO_FLUSH : Z_FINISH);
    sync_out(z, scb);
    r[2] = ros_ld32(scb + ZM_NEXT_IN);
    r[3] = ros_ld32(scb + ZM_AVAIL_IN);
    r[4] = ros_ld32(scb + ZM_NEXT_OUT);
    r[5] = ros_ld32(scb + ZM_AVAIL_OUT);
    switch (ret) {
    case Z_OK:
    case Z_BUF_ERROR:
        r[0] = (r[3] || !more) ? 2u : 1u;   /* output full : wants input */
        return NULL;
    case Z_STREAM_END:
        r[0] = 0;
        break;
    default:
        e = zm_zlib_error(ret, z->zs.msg);
        break;
    }
    if (compress)
        deflateEnd(&z->zs);
    else
        inflateEnd(&z->zs);
    settle(z, scb);
    return e;
}

/* ---- the one-shot calls --------------------------------------------------------------- */

/* ZCompress, ZCompress2, ZUncompress: R0 -> output, R1 its size; R2 ->
 * input, R3 its size; R4 the level (ZCompress2 only). Out R0 the code, R1
 * the output's length. ZUncompress2: R3 -> a word holding the input's
 * size, which is updated. */
static os_error *one_shot(uint32_t *r, int which)
{
    uint32_t insize = r[3];
    if (which == 3) {
        if (!zm_valid(r[3], 4))
            return zm_abort(r[3]);
        insize = ros_ld32(r[3]);
    }
    if (!zm_valid(r[0], r[1]))
        return zm_abort(r[0]);
    if (!zm_valid(r[2], insize))
        return zm_abort(r[2]);
    uLongf outlen = r[1];
    uLong srclen = insize;
    int ret;
    switch (which) {
    case 0: ret = compress(ptr_or_null(r[0]), &outlen, ptr_or_null(r[2]), srclen); break;
    case 1: ret = compress2(ptr_or_null(r[0]), &outlen, ptr_or_null(r[2]), srclen, (int)r[4]); break;
    case 2: ret = uncompress(ptr_or_null(r[0]), &outlen, ptr_or_null(r[2]), srclen); break;
    default:
        ret = uncompress2(ptr_or_null(r[0]), &outlen, ptr_or_null(r[2]), &srclen);
        ros_st32(r[3], (uint32_t)srclen);
        break;
    }
    r[0] = (uint32_t)ret;
    r[1] = (uint32_t)outlen;
    return NULL;
}

/* CRC32 and Adler32: R0 the value so far, R1 -> the start, R2 -> the end */
static os_error *checksum(uint32_t *r, int crc)
{
    uint32_t len = r[2] - r[1];
    if (r[1] && !zm_valid(r[1], len))
        return zm_abort(r[1]);
    const Bytef *p = ptr_or_null(r[1]);
    r[0] = (uint32_t)(crc ? crc32(r[0], p, len) : adler32(r[0], p, len));
    return NULL;
}

/* TaskAssociate: R0 -> the block. R1 is 0 to let the stream outlive its
 * task, or 1 to have it ended with the task again. Registers are kept. */
static os_error *task_associate(uint32_t *r)
{
    if (r[1] > 1)
        return zm_error(ZM_E_INVTA);
    uint32_t t = current_task();
    if (!t || !r[0] || !zm_valid(r[0], ZM_SCB_SIZE))
        return NULL;
    struct zrec *z = rec_of(r[0]);
    if (z && z->task == t)
        z->active = (int)r[1];
    return NULL;
}

/* ---- the SWIs --------------------------------------------------------------------------- */

static void dispatch(struct ros_cpu *s, uint32_t offset)
{
    uint32_t *r = s->r;
    os_error *e = NULL;
    switch (offset) {
    case 0: e = squash_like(r, 1); break;
    case 1: e = squash_like(r, 0); break;
    case 2: e = checksum(r, 1); break;
    case 3: e = checksum(r, 0); break;
    case 4: r[0] = ros_addr(zm_ws()->version); break;
    case 5: e = one_shot(r, 0); break;
    case 6: e = one_shot(r, 1); break;
    case 7: e = one_shot(r, 2); break;
    case 8: e = init_stream(r, DEFLATE_INIT); break;
    case 9: e = init_stream(r, INFLATE_INIT); break;
    case 10: e = init_stream(r, DEFLATE_INIT2); break;
    case 11: e = init_stream(r, INFLATE_INIT2); break;
    case 12: e = stream_call(r, OP_DEFLATE); break;
    case 13: e = stream_call(r, OP_DEFLATE_END); break;
    case 14: e = stream_call(r, OP_INFLATE); break;
    case 15: e = stream_call(r, OP_INFLATE_END); break;
    case 16: e = stream_call(r, OP_DEFLATE_SET_DICT); break;
    case 17: e = deflate_copy(r); break;
    case 18: e = stream_call(r, OP_DEFLATE_RESET); break;
    case 19: e = stream_call(r, OP_DEFLATE_PARAMS); break;
    case 20: e = stream_call(r, OP_INFLATE_SET_DICT); break;
    case 21: e = stream_call(r, OP_INFLATE_SYNC); break;
    case 22: e = stream_call(r, OP_INFLATE_RESET); break;
    case 23: e = zm_gz_open(s); break;
    case 24: e = zm_gz_read(s); break;
    case 25: e = zm_gz_write(s); break;
    case 26: e = zm_gz_flush(s); break;
    case 27: e = zm_gz_close(s); break;
    case 28: e = zm_gz_error(s); break;
    case 29: e = zm_gz_seek(s); break;
    case 30: e = zm_gz_tell(s); break;
    case 31: e = zm_gz_eof(s); break;
    case 32: e = task_associate(r); break;
    case 33: e = stream_call(r, OP_INFLATE_RESET2); break;
    case 34: e = one_shot(r, 3); break;
    case 35: e = stream_call(r, OP_DEFLATE_PENDING); break;
    case 36: e = stream_call(r, OP_DEFLATE_GET_DICT); break;
    case 37: e = stream_call(r, OP_INFLATE_GET_DICT); break;
    default: e = zm_error(ZM_E_UKSWI); break;
    }
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

#define THUNK(name, n) void ros_thunk_ZLib_##name(struct ros_cpu *s) { dispatch(s, n); }
THUNK(Compress, 0) THUNK(Decompress, 1) THUNK(CRC32, 2) THUNK(Adler32, 3)
THUNK(Version, 4) THUNK(ZCompress, 5) THUNK(ZCompress2, 6) THUNK(ZUncompress, 7)
THUNK(DeflateInit, 8) THUNK(InflateInit, 9) THUNK(DeflateInit2, 10) THUNK(InflateInit2, 11)
THUNK(Deflate, 12) THUNK(DeflateEnd, 13) THUNK(Inflate, 14) THUNK(InflateEnd, 15)
THUNK(DeflateSetDictionary, 16) THUNK(DeflateCopy, 17) THUNK(DeflateReset, 18)
THUNK(DeflateParams, 19) THUNK(InflateSetDictionary, 20) THUNK(InflateSync, 21)
THUNK(InflateReset, 22) THUNK(GZOpen, 23) THUNK(GZRead, 24) THUNK(GZWrite, 25)
THUNK(GZFlush, 26) THUNK(GZClose, 27) THUNK(GZError, 28) THUNK(GZSeek, 29)
THUNK(GZTell, 30) THUNK(GZEOF, 31) THUNK(TaskAssociate, 32) THUNK(InflateReset2, 33)
THUNK(ZUncompress2, 34) THUNK(DeflatePending, 35) THUNK(DeflateGetDictionary, 36)
THUNK(InflateGetDictionary, 37)

/* SWIs 38-63 of the chunk: c/cmodule's default */
static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return zm_error(ZM_E_UKSWI);
}

/* ---- the module -------------------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct zm_workspace *w;
    if (xos_module_claim(sizeof *w, (void **)&w))
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    snprintf(w->version, sizeof w->version, "%s", zlibVersion());
    ros_st32(m->private_word, ros_addr(w));
    return NULL;
}

/* Every stream and file goes with the module. The blocks in the programs'
 * memory then name records that no longer exist, and calls on them give
 * Z_STREAM_ERROR. */
static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct zm_workspace *w = zm_ws();
    zm_gz_close_all();
    while (w->streams) {
        struct zrec *z = ros_ptr(w->streams);
        if (z->isdeflate)
            deflateEnd(&z->zs);
        else
            inflateEnd(&z->zs);
        free_rec(z);
    }
    xos_module_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

/* Service_WimpCloseDown (R0 0: the task itself, R2 its handle). The
 * streams associated with the task end. The task's other streams stay,
 * but they lose their task (c/task's task_close). */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] != 0x53 || s->r[0] != 0 || !zlib_module.private_word ||
        !ros_ld32(zlib_module.private_word))
        return;
    uint32_t task = s->r[2];
    uint32_t a = zm_ws()->streams;
    while (a) {
        struct zrec *z = ros_ptr(a);
        a = z->next;
        if (z->task != task)
            continue;
        if (z->active) {
            if (z->isdeflate)
                deflateEnd(&z->zs);
            else
                inflateEnd(&z->zs);
            free_rec(z);
        } else {
            z->task = 0;
        }
    }
}

struct ros_module zlib_module = {
    .title = "ZLib",
    .help = "ZLib\t\t0.05 (28 May 2022) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x53AC0,
    .swi_thunks = ros_swi_thunks_ZLib,
    .swi_names = ros_swi_names_ZLib,
    .swi_prefix = "ZLib",
};

__attribute__((constructor)) static void count(void)
{
    zlib_module.swi_count = ros_swi_count_ZLib;
}
