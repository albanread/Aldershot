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
 * This file is a reimplementation in C of RISC OS Open's ARM assembler source
 * (Sources/Video/Render/Fonts/FontManager: s.ListFonts, s.Fonts01).
 */

/* catalogue.c: the fonts and encodings that Font$Path offers (s/ListFonts).
 *
 * Font$Path is decoded into prefixes. Each prefix is scanned once. If it
 * has a Messages<territory> file, the scan reads the Font_*, LFont_*,
 * Encoding_* and BEncoding_* tokens from that. Otherwise it walks the
 * prefix's directories. The scan makes blocks of identifier, name and
 * flags. The blocks are kept sorted by name and then by prefix.
 *
 * Font_ListFonts lists the blocks, or builds Wimp menus from them.
 * Font_DecodeMenu turns a menu selection back into an identifier. Every
 * font or encoding that a font string names is found here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fm.h"

struct fm_block *fm_catalogue;
static struct fm_prefix *prefixes;
uint32_t fm_territory = 1;

/* ---- character helpers ---------------------------------------------------------------------- */

uint8_t fm_lower(uint8_t c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

uint8_t fm_lower_latin1(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE))
        return c + 32;
    return c;
}

/* strcmp_nocase: returns a negative, zero or positive number */
static int strcmp_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = fm_lower_latin1((uint8_t)*a), y = fm_lower_latin1((uint8_t)*b);
        if (x != y)
            return x - y;
        if (!x)
            return 0;
    }
}

/* comparefontid_gotfield: compares the block's identifier (ended by 0 or
 * CR) with a candidate (ended by '\', a space or a control character).
 * A to Z are folded to lower case. */
int fm_compare_id(const char *id, const char *cand)
{
    for (;; id++, cand++) {
        int x = (uint8_t)*id, y = (uint8_t)*cand;
        if (y == '\\' || y <= ' ')
            y = 0;
        if (x == 13)
            x = 0;
        x = fm_lower((uint8_t)x), y = fm_lower((uint8_t)y);
        if (x != y)
            return x - y;
        if (!x)
            return 0;
    }
}

/* findfield */
const char *fm_find_field(const char *s, int c)
{
    const char *p = s;
    int term = ' ';
    if (*p != '\\') {
        if (c == 'F')
            return s;
    } else {
        p++;
        goto field;
    }
    for (;;) {
        int ch = (uint8_t)*p++;
        if (ch == '\\') {
field:
            term = ' ' - 1;
            if ((uint8_t)*p++ == c)
                return p;
            continue;
        }
        if (ch <= term)
            return NULL;
    }
}

/* ---- Font$Path's prefixes -------------------------------------------------------------------- */

static os_error *gstrans_path(char *out, size_t size)
{
    uint32_t in = ros_addr(ros_rma_alloc(16)), buf = ros_addr(ros_rma_alloc((uint32_t)size));
    memcpy(ros_ptr(in), "<Font$Path>", 12);
    uint32_t r[10] = { in, buf, (uint32_t)size };
    os_error *e;
    uint32_t c = fm_swi(XOS_GSTrans, r, &e);
    if (!e && c)
        e = fm_err(FE_BUFFOVERFLOW, NULL, NULL);
    if (!e) {
        memcpy(out, ros_ptr(buf), r[2]);
        out[r[2]] = 0;
    }
    ros_rma_free(ros_ptr(in)), ros_rma_free(ros_ptr(buf));
    return e;
}

/* decodefontpath: splits the path into elements.  Spaces are removed, and
 * empty elements and "." are dropped. */
static os_error *decode_path(struct fm_prefix **list)
{
    char path[284];
    os_error *e = gstrans_path(path, sizeof path - 1);
    if (e)
        return e;
    struct fm_prefix *head = NULL, **tail = &head;
    for (const char *p = path; *p;) {
        char el[256];
        size_t n = 0;
        for (; *p && *p != ','; p++)
            if (*p != ' ' && n < sizeof el - 1)
                el[n++] = *p;
        if (*p == ',')
            p++;
        el[n] = 0;
        if (!n || !strcmp(el, "."))
            continue;
        struct fm_prefix *x = calloc(1, sizeof *x);
        strcpy(x->prefix, el);
        *tail = x, tail = &x->next;
    }
    *list = head;
    return NULL;
}

static int prefix_pos(const struct fm_prefix *p)
{
    int i = 0;
    for (struct fm_prefix *x = prefixes; x && x != p; x = x->next)
        i++;
    return i;
}

/* ---- the blocks ------------------------------------------------------------------------------- */

static void free_block(struct fm_block *b)
{
    if (b->name != b->id)
        free(b->name);
    free(b->id);
    free(b);
}

/* makefontcatblock: adds the block at the head of the list */
static void make_block(struct fm_prefix *pre, const char *id, const char *name, uint32_t flags)
{
    struct fm_block *b = calloc(1, sizeof *b);
    b->id = strdup(id);
    size_t n = strlen(name);
    if (n && name[n - 1] == '*')
        flags |= LFFF_DEFAULT, n--;
    if (!n) {
        b->name = b->id;
    } else {
        b->name = malloc(n + 1);
        memcpy(b->name, name, n);
        b->name[n] = 0;
    }
    b->territory = b->name == b->id ? 0 : fm_territory;
    b->flags = flags, b->prefix = pre;
    b->next = fm_catalogue, fm_catalogue = b;
}

/* sortfontblocks: sorts by name, then by the prefix's place in the path.
 * The sort is stable. */
static void sort_blocks(void)
{
    int changed = 1;
    while (changed) {
        changed = 0;
        for (struct fm_block **pp = &fm_catalogue; *pp && (*pp)->next; pp = &(*pp)->next) {
            struct fm_block *a = *pp, *b = a->next;
            int c = strcmp_nocase(a->name, b->name);
            if (c > 0 || (c == 0 && prefix_pos(a->prefix) > prefix_pos(b->prefix))) {
                a->next = b->next, b->next = a, *pp = b;
                changed = 1;
            }
        }
    }
}

/* ---- scanning a prefix ------------------------------------------------------------------------ */

static os_error *scan_messages(struct fm_prefix *pre, int *found)
{
    char file[300];
    snprintf(file, sizeof file, "%sMessages%u", pre->prefix, fm_territory);
    uint32_t desc = ros_addr(ros_rma_alloc(16)), name = ros_addr(ros_rma_alloc(304));
    uint32_t tok = ros_addr(ros_rma_alloc(64)), val = ros_addr(ros_rma_alloc(128));
    uint32_t pat = ros_addr(ros_rma_alloc(16));
    strcpy(ros_ptr(name), file);
    uint32_t r[10] = { desc, name, 0 };
    os_error *e;
    fm_swi(XMessageTrans_OpenFile, r, &e);
    *found = !e;
    if (!e) {
        static const struct { const char *wild; uint32_t flags; int slash; } kinds[4] = {
            { "Font_*", LFFF_SYMFONT, 0 }, { "LFont_*", LFFF_LANGFONT, 0 },
            { "Encoding_*", LFFF_ENCODING, 0 }, { "BEncoding_*", LFFF_ENCODING, 1 },
        };
        for (int k = 0; k < 4; k++) {
            strcpy(ros_ptr(pat), kinds[k].wild);
            uint32_t index = 0;
            for (;;) {
                uint32_t q[10] = { desc, pat, tok, 64, index };
                fm_swi(XMessageTrans_EnumerateTokens, q, &e);
                if (e || !q[2])
                    break;
                index = q[4];
                char id[70];
                const char *t = strchr(ros_ptr(tok), '_');
                snprintf(id, sizeof id, "%s%s", kinds[k].slash ? "/" : "", t ? t + 1 : "");
                uint32_t l[10] = { desc, tok, val, 128 };
                fm_swi(XMessageTrans_Lookup, l, &e);
                char value[130] = "";
                if (!e) {
                    size_t n = l[3] < 128 ? l[3] : 127;
                    memcpy(value, ros_ptr(l[2]), n);
                    value[n] = 0;
                }
                e = NULL;
                make_block(pre, id, value, kinds[k].flags);
            }
        }
        uint32_t c[10] = { desc };
        fm_swi(XMessageTrans_CloseFile, c, &e);
        e = NULL;
    } else {
        e = NULL;
    }
    ros_rma_free(ros_ptr(desc)), ros_rma_free(ros_ptr(name)), ros_rma_free(ros_ptr(tok));
    ros_rma_free(ros_ptr(val)), ros_rma_free(ros_ptr(pat));
    return e;
}

/* strcmp_trailingdigits(name, "IntMetrics") */
static int is_langfont_name(const char *name)
{
    static const char *im = "IntMetrics";
    size_t i = 0;
    while (im[i] && fm_lower((uint8_t)name[i]) == fm_lower((uint8_t)im[i]))
        i++;
    if (!im[i] && (uint8_t)name[i] <= ' ')
        return 0;                       /* exactly IntMetrics, which is a symbol font */
    size_t n = strlen(name);
    const char *rest = name[i] ? name + i + 1 : name + i;
    for (const char *p = rest; (uint8_t)*p > ' '; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return !im[i] || n == 10;
}

/* The directory scan.  Font directories hold IntMetrics or IntMetric<n>.
 * The Encodings directory holds encoding files. */
static os_error *scan_dir(struct fm_prefix *pre, const char *rel, int encodings, int depth)
{
    if (depth > 16)
        return fm_err(FE_BUFFOVERFLOW, NULL, NULL);
    char dir[300];
    snprintf(dir, sizeof dir, "%s%s", pre->prefix, rel);
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '.')
        dir[n - 1] = 0;
    uint32_t dname = ros_addr(ros_rma_alloc(304)), buf = ros_addr(ros_rma_alloc(256));
    strcpy(ros_ptr(dname), dir);
    uint32_t flags = 0, offset = 0;
    os_error *e = NULL;
    while (!e && offset != 0xFFFFFFFFu) {
        uint32_t r[10] = { 10, dname, buf, 1, offset, 256, 0 };
        fm_swi(XOS_GBPB, r, &e);
        if (e)
            break;
        offset = r[4];
        if (!r[3])
            continue;
        uint32_t type = ros_ld32(buf + 16);
        const char *leaf = ros_ptr(buf + 20);
        char sub[300];
        snprintf(sub, sizeof sub, "%s%s", rel, leaf);
        if (type == 1) {
            if (encodings) {
                make_block(pre, sub + strlen("Encodings."), "", LFFF_ENCODING);
            } else if (!strcmp_nocase(leaf, "IntMetrics")) {
                flags |= LFFF_SYMFONT;
            } else if (is_langfont_name(leaf)) {
                flags |= LFFF_LANGFONT;
            }
        } else if (type & 2) {
            char next[300];
            snprintf(next, sizeof next, "%s.", sub);
            int enc = encodings || (!*rel && !strcmp_nocase(leaf, "Encodings"));
            e = scan_dir(pre, next, enc, depth + 1);
        }
    }
    if (!e && flags && *rel) {
        char id[300];
        snprintf(id, sizeof id, "%s", rel);
        size_t k = strlen(id);
        if (k && id[k - 1] == '.')
            id[k - 1] = 0;
        make_block(pre, id, "", flags & LFFF_LANGFONT ? LFFF_LANGFONT : LFFF_SYMFONT);
    }
    ros_rma_free(ros_ptr(dname)), ros_rma_free(ros_ptr(buf));
    return e;
}

static os_error *scan_prefix(struct fm_prefix *pre)
{
    if (pre->prefix[0] == '.')
        return NULL;
    int found;
    os_error *e = scan_messages(pre, &found);
    if (!e && !found)
        e = scan_dir(pre, "", 0, 0);
    return e;
}

/* ---- try_listfonts_recache ------------------------------------------------------------------- */

static int same_list(struct fm_prefix *a, struct fm_prefix *b)
{
    for (; a && b; a = a->next, b = b->next)
        if (strcmp(a->prefix, b->prefix))
            return 0;
    return !a && !b;
}

os_error *fm_recache(void)
{
    struct fm_prefix *now;
    os_error *e = decode_path(&now);
    if (e)
        return e;
    int changed = 0;
    if (same_list(now, prefixes)) {
        while (now) {
            struct fm_prefix *x = now->next;
            free(now);
            now = x;
        }
    } else {
        /* Carry each old prefix's state over to an equal new one.  Drop the
         * blocks of the prefixes that have gone. */
        for (struct fm_prefix *o = prefixes; o; o = o->next) {
            struct fm_prefix *m = NULL;
            for (struct fm_prefix *x = now; x && !m; x = x->next)
                if (!strcmp_nocase(x->prefix, o->prefix))
                    m = x;
            if (m) {
                m->flags = o->flags;
                for (struct fm_block *b = fm_catalogue; b; b = b->next)
                    if (b->prefix == o)
                        b->prefix = m;
            } else {
                changed = 1;
                for (struct fm_block **pp = &fm_catalogue; *pp;) {
                    if ((*pp)->prefix == o) {
                        struct fm_block *d = *pp;
                        *pp = d->next;
                        free_block(d);
                    } else {
                        pp = &(*pp)->next;
                    }
                }
            }
        }
        while (prefixes) {
            struct fm_prefix *x = prefixes->next;
            free(prefixes);
            prefixes = x;
        }
        prefixes = now;
        sort_blocks();
        changed = 1;
    }
    for (struct fm_prefix *p = prefixes; p && !e; p = p->next) {
        if (p->flags & (LFPR_SCANNED | LFPR_ISBAD))
            continue;
        changed = 1;
        e = scan_prefix(p);
        p->flags |= e ? LFPR_ISBAD : LFPR_SCANNED;
        sort_blocks();
        if (e)
            e = fm_err(FE_BADPREFIX, NULL, e->errmess);
    }
    if (changed) {
        char keep[256];
        uint32_t num = e ? e->errnum : 0;
        if (e)
            snprintf(keep, sizeof keep, "%s", e->errmess);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = 0x6E;                  /* Service_FontsChanged */
        ros_service_call(&s);
        if (num)
            e = ros_error(num, "%s", keep);
    }
    return e;
}

/* ---- lookups ---------------------------------------------------------------------------------- */

static struct fm_block *find_in_list(const char *id, int encoding)
{
    for (struct fm_block *b = fm_catalogue; b; b = b->next)
        if (!!(b->flags & LFFF_ENCODING) == !!encoding && !fm_compare_id(b->id, id))
            return b;
    return NULL;
}

os_error *fm_find_block(const char *id, int encoding, struct fm_block **b)
{
    if ((*b = find_in_list(id, encoding)))
        return NULL;
    os_error *e = fm_recache();
    if (e)
        return e;
    if ((*b = find_in_list(id, encoding)))
        return NULL;
    char name[256];
    fm_name_from_id(id, 0, name, sizeof name);          /* (this gives a font's name for an encoding too) */
    return fm_err(encoding ? FE_ENCNOTFOUND : FE_NOTFOUND, name, NULL);
}

os_error *fm_find_block_field(const char *s, int encoding, struct fm_block **b)
{
    const char *f = fm_find_field(s, encoding ? 'E' : 'F');
    return fm_find_block(f ? f : s, encoding, b);
}

/* get_name_from_id: gives the name if its territory is ours.  Otherwise it
 * gives the name in the string's \f (or \e) field if that is in our
 * territory.  Otherwise it gives the identifier. */
void fm_name_from_id(const char *s, int encoding, char *out, size_t size)
{
    static int semaphore;
    out[0] = 0;
    if (!s || semaphore)
        return;
    semaphore = 1;
    const char *id = s;
    struct fm_block *b = find_in_list(s, encoding);
    if (b && b->territory == fm_territory) {
        snprintf(out, size, "%s", b->name);
        semaphore = 0;
        return;
    }
    const char *f = fm_find_field(s, encoding ? 'e' : 'f');
    if (f) {
        char *end;
        unsigned long t = strtoul(f, &end, 10);
        if (end != f && t == fm_territory) {
            size_t n = 0;
            for (const char *p = end + 1; (uint8_t)*p >= ' ' && *p != '\\' && n + 1 < size; p++)
                out[n++] = *p;
            out[n] = 0;
            semaphore = 0;
            return;
        }
    }
    if (b)
        id = b->id;
    size_t n = 0;
    for (const char *p = id; (uint8_t)*p > ' ' && *p != '\\' && n + 1 < size; p++)
        out[n++] = *p;
    out[n] = 0;
    semaphore = 0;
}

/* ---- Font_ListFonts ---------------------------------------------------------------------------- */

enum {
    LF_RETURNID = 1 << 16, LF_RETURNNAME = 1 << 17, LF_RETURNCR = 1 << 18, LF_RETURNMENU = 1 << 19,
    LF_SYSTEMFONT = 1 << 20, LF_TICKFONT = 1 << 21, LF_ENCODINGS = 1 << 22,
};
#define LF_RESERVED 0xFF800000u

static struct fm_block *first_font(uint32_t r2)
{
    int want = !!(r2 & LF_ENCODINGS);
    struct fm_block *b = fm_catalogue;
    while (b && (!!(b->flags & LFFF_ENCODING) != want || b->name[0] == '/'))
        b = b->next;
    return b;
}

static struct fm_block *next_font(struct fm_block *cur)
{
    int kind = !!(cur->flags & LFFF_ENCODING);
    for (struct fm_block *b = cur->next; b; b = b->next) {
        if (!!(b->flags & LFFF_ENCODING) != kind)
            continue;
        if (strcmp_nocase(cur->id, b->id) && b->name[0] != '/')
            return b;
    }
    return NULL;
}

static uint32_t count(const char *s)                  /* count_R0 */
{
    uint32_t n = 0;
    while (s[n] && s[n] != 10 && s[n] != 13)
        n++;
    return n + 1;
}

/* LookupR0: looks the token up in the Font Manager's Messages.  If that
 * fails it gives the default, which follows the ':'. */
static const char *lookup(const char *tokdef, char *out, size_t size)
{
    const char *colon = strchr(tokdef, ':');
    snprintf(out, size, "%s", colon ? colon + 1 : tokdef);
    uint32_t desc = ros_addr(ros_rma_alloc(16)), name = ros_addr(ros_rma_alloc(64));
    uint32_t tok = ros_addr(ros_rma_alloc(64)), val = ros_addr(ros_rma_alloc(128));
    strcpy(ros_ptr(name), "Resources:$.Resources.Fonts.Messages");
    uint32_t r[10] = { desc, name, 0 };
    os_error *e;
    fm_swi(XMessageTrans_OpenFile, r, &e);
    if (!e) {
        size_t n = colon ? (size_t)(colon - tokdef) : strlen(tokdef);
        memcpy(ros_ptr(tok), tokdef, n);
        ((char *)ros_ptr(tok))[n] = 0;
        uint32_t l[10] = { desc, tok, val, 128 };
        fm_swi(XMessageTrans_Lookup, l, &e);
        if (!e) {
            size_t k = l[3] < size - 1 ? l[3] : size - 1;
            memcpy(out, ros_ptr(l[2]), k);
            out[k] = 0;
        }
        uint32_t c[10] = { desc };
        fm_swi(XMessageTrans_CloseFile, c, &e);
    }
    ros_rma_free(ros_ptr(desc)), ros_rma_free(ros_ptr(name));
    ros_rma_free(ros_ptr(tok)), ros_rma_free(ros_ptr(val));
    return out;
}

/* strcmp_dot: returns 1 if a and b are equal up to a '.' or the end.
 * *len is the length of a up to and including its first '.' or its
 * terminator. */
static int strcmp_dot(const char *a, const char *b, uint32_t *len)
{
    size_t i = 0;
    for (;; i++) {
        int x = (uint8_t)a[i], y = (uint8_t)b[i];
        if (x == '.') x = 0;
        if (y == '.') y = 0;
        if (x != y)
            break;
        if (!x) {
            *len = (uint32_t)i + 1;
            return 1;
        }
    }
    int x = (uint8_t)a[i];
    while (x != '.' && x != 0)
        x = (uint8_t)a[++i];
    *len = (uint32_t)i + 1;
    return 0;
}

/* The menu under construction */
struct lfmenu {
    uint32_t r1, r3, r4, r5, r2, r6;    /* the menu buffer and its size, the text buffer
                                           and its size, the flags, the font to tick */
    uint32_t used1, used4;              /* R8, R9: the space used in each buffer */
    uint32_t menu;                      /* R11: the current menu */
    uint32_t parent;                    /* lf_menuparent: just after the parent item */
    int indirectable, indirected;
    os_error *e;
};

static void put_text(struct lfmenu *m, uint32_t at, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        ros_st8(at + i, (uint8_t)s[i]);
    ros_st8(at + n - 1, m->r2 & LF_RETURNCR ? 13 : 0);
}

static void new_menu(struct lfmenu *m, const char *title, uint32_t len)
{
    m->used1 += 28;
    m->indirected = len > 13 && m->indirectable;
    if (m->indirected)
        m->used4 += len;
    if (!m->r1 || m->e)
        return;
    m->menu = m->r1;
    if ((m->indirected && m->used4 > m->r5) || m->used1 > m->r3) {
        m->e = fm_err(FE_BUFFOVERFLOW, NULL, NULL);
        return;
    }
    if (m->indirected) {
        ros_st32(m->r1, m->r4), ros_st32(m->r1 + 4, 0), ros_st32(m->r1 + 8, 0);
        put_text(m, m->r4, title, len);
        m->r4 += len;
    } else {
        uint32_t n = len > 12 ? 12 : len;
        for (uint32_t i = 0; i < n; i++)
            ros_st8(m->r1 + i, (uint8_t)title[i]);
        if (len <= 12)
            ros_st8(m->r1 + n - 1, m->r2 & LF_RETURNCR ? 13 : 0);
    }
    ros_st32(m->r1 + 12, 0x00070207);   /* 7,2,7,0 */
    ros_st32(m->r1 + 16, len * 16 - 36);
    ros_st32(m->r1 + 20, 44);
    ros_st32(m->r1 + 24, 0);
    m->r1 += 28;
}

static void new_item(struct lfmenu *m, const char *text, uint32_t len, struct fm_block *b)
{
    m->used1 += 24, m->used4 += len;
    if (!m->r1 || m->e)
        return;
    if (m->used1 > m->r3 || m->used4 > m->r5) {
        m->e = fm_err(FE_BUFFOVERFLOW, NULL, NULL);
        return;
    }
    uint32_t w = len * 16 - 4;
    if ((int32_t)ros_ld32(m->menu + 16) < (int32_t)w)
        ros_st32(m->menu + 16, w);
    int tick = 0;
    if (m->r2 & LF_TICKFONT) {
        if (m->r6 == 1 && !b) {
            tick = 1;
        } else if (m->r6 >= 0x8000 && b) {
            const char *want = fm_find_field(ros_ptr(m->r6), m->r2 & LF_ENCODINGS ? 'E' : 'F');
            tick = want && !fm_compare_id(b->id, want);
        }
    }
    uint32_t flags = tick ? 1 : 0;
    if (m->parent && flags)
        ros_st32(m->parent - 24, ros_ld32(m->parent - 24) | flags);
    if (m->indirectable && m->indirected)
        flags |= 0x100;
    ros_st32(m->r1, flags), ros_st32(m->r1 + 4, 0xFFFFFFFFu), ros_st32(m->r1 + 8, 0x07000101u);
    ros_st32(m->r1 + 12, m->r4), ros_st32(m->r1 + 16, 0xFFFFFFFFu), ros_st32(m->r1 + 20, len);
    m->r1 += 24;
    put_text(m, m->r4, text, len);
    m->r4 += len;
}

static void last_item(struct lfmenu *m)
{
    if (m->r1)
        ros_st32(m->r1 - 24, ros_ld32(m->r1 - 24) | 0x80);
}

static os_error *list_menu(struct ros_cpu *s)
{
    struct lfmenu m = { .r1 = s->r[1], .r3 = s->r[3], .r4 = s->r[4], .r5 = s->r[5],
                        .r2 = s->r[2], .r6 = s->r[6] };
    uint32_t r[10] = { 7 };
    os_error *e;
    fm_swi(XWimp_ReadSysInfo, r, &e);
    m.indirectable = !e && r[0] >= 310;
    if (s->r[2] & 0xFFFF)
        return fm_err(FE_BADPARM, NULL, NULL);
    if (!(s->r[2] & LF_SYSTEMFONT) && !first_font(s->r[2])) {
        s->r[3] = 0, s->r[5] = 0;
        return NULL;
    }
    char title[64], sys[64], regular[64];
    lookup("FontList:Font List", title, sizeof title);
    lookup("SystemFont:System font", sys, sizeof sys);
    lookup("Regular:(Regular)", regular, sizeof regular);
    new_menu(&m, title, count(title));
    uint32_t top = s->r[1];
    if (s->r[2] & LF_SYSTEMFONT)
        new_item(&m, sys, count(sys), NULL);
    const char *prev = "";
    for (struct fm_block *b = first_font(s->r[2]); b && !m.e; b = next_font(b)) {
        uint32_t len;
        if (!strcmp_dot(b->name, prev, &len)) {
            prev = b->name;
            new_item(&m, b->name, len, b);
        }
    }
    last_item(&m);
    /* pass 2: the submenus */
    m.parent = top ? top + 28 + (s->r[2] & LF_SYSTEMFONT ? 24 : 0) : 0;
    prev = s->r[2] & LF_SYSTEMFONT ? sys : "";
    for (struct fm_block *b = first_font(s->r[2]); b && !m.e; b = next_font(b)) {
        uint32_t len;
        if (!strcmp_dot(b->name, prev, &len)) {
            last_item(&m);
            prev = b->name;
            new_menu(&m, b->name, len);
            if (m.parent)
                m.parent += 24;
        }
        const char *rest = b->name + len;
        int reg = b->name[len - 1] == 0;
        if (reg)
            rest = regular;
        else if (m.r1 && m.parent)
            ros_st32(m.parent - 24 + 4, m.menu);
        new_item(&m, rest, count(rest), b);
    }
    last_item(&m);
    if (m.e)
        return m.e;
    s->r[3] = m.used1, s->r[5] = m.used4;
    return NULL;
}

void fm_thunk_ListFonts(struct ros_cpu *s)
{
    uint32_t in2 = s->r[2], r2 = s->r[2];
    if (!(r2 >> 16))
        r2 |= LF_RETURNID | LF_RETURNCR, s->r[3] = 40;
    uint32_t size1 = s->r[3];           /* the buffer size that the check uses */
    uint32_t bad = r2 & LF_RETURNMENU ? LF_RETURNID | LF_RETURNNAME | LF_RESERVED
                                      : LF_RETURNMENU | LF_SYSTEMFONT | LF_TICKFONT | LF_RESERVED;
    os_error *e = r2 & bad ? fm_err(FE_BADPARM, NULL, NULL) : fm_recache();
    if (!e && (r2 & LF_RETURNMENU)) {
        s->r[2] = r2;
        e = list_menu(s);
        s->r[2] = e ? 0xFFFFFFFFu : in2;
        if (e) {
            ros_swi_fail(s, e);
            return;
        }
        s->v = 0;
        return;
    }
    if (!e) {
        struct fm_block *b = first_font(r2);
        int32_t skip = (int32_t)(r2 << 16);
        while (b && skip > 0) {
            b = next_font(b);
            skip -= 0x10000;
        }
        if (!b) {
            r2 = 0xFFFFFFFFu;
        } else {
            if (r2 & LF_RETURNID) {
                uint32_t n = count(b->id);
                if (in2 >> 16)
                    s->r[3] = n;
                if (s->r[1]) {
                    if (n > size1) {
                        e = fm_err(FE_BUFFOVERFLOW, NULL, NULL);
                    } else {
                        for (uint32_t i = 0; i < n; i++)
                            ros_st8(s->r[1] + i, (uint8_t)b->id[i]);
                        if (r2 & LF_RETURNCR)
                            ros_st8(s->r[1] + n - 1, 13);
                    }
                }
            }
            if (!e && (r2 & LF_RETURNNAME)) {
                uint32_t n = count(b->name), size = s->r[5];
                s->r[5] = n;
                if (s->r[4]) {
                    if (n > size) {
                        e = fm_err(FE_BUFFOVERFLOW, NULL, NULL);
                    } else {
                        for (uint32_t i = 0; i < n; i++)
                            ros_st8(s->r[4] + i, (uint8_t)b->name[i]);
                        if (r2 & LF_RETURNCR)
                            ros_st8(s->r[4] + n - 1, 13);
                    }
                }
            }
            if (!e)
                r2++;
        }
    }
    if (e)
        r2 = 0xFFFFFFFFu;
    if (!(in2 >> 16) && !(r2 & 0x80000000u))
        r2 &= ~(uint32_t)(LF_RETURNID | LF_RETURNCR);
    s->r[2] = r2;
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

/* ---- Font_DecodeMenu ---------------------------------------------------------------------------- */

/* getmenuitem: an item's text, up to its first control character */
static void item_text(uint32_t menu, int32_t sel, char *out, size_t size)
{
    uint32_t item = menu + 28 + 24 * (uint32_t)sel;
    uint32_t flags = ros_ld32(item + 8);
    size_t n = 0;
    if (flags & 0x100) {                /* indirected */
        uint32_t p = ros_ld32(item + 12);
        for (uint8_t c; n + 1 < size && (c = (uint8_t)ros_ld8(p + n)) >= ' ';)
            out[n++] = (char)c;
    } else {
        for (uint8_t c; n < 12 && n + 1 < size && (c = (uint8_t)ros_ld8(item + 12 + n)) >= ' ';)
            out[n++] = (char)c;
    }
    out[n] = 0;
}

void fm_thunk_DecodeMenu(struct ros_cpu *s)
{
    if (s->r[0] & ~1u) {
        ros_swi_fail(s, fm_err(FE_BADPARM, NULL, NULL));
        return;
    }
    int enc = s->r[0] & 1;
    os_error *e = fm_recache();
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    uint32_t sel = s->r[2];
    char want[256], t[128];
    int32_t s0 = (int32_t)ros_ld32(sel), s1 = (int32_t)ros_ld32(sel + 4);
    item_text(s->r[1], s0, t, sizeof t);
    snprintf(want, sizeof want, "%s", t);
    uint32_t used = 4;
    if (s1 != -1) {
        uint32_t sub = ros_ld32(s->r[1] + 28 + 24 * (uint32_t)s0 + 4);
        item_text(sub, s1, t, sizeof t);
        size_t k = strlen(want);
        snprintf(want + k, sizeof want - k, ".%s", t);
        used = 8;
    }
    char regular[64];
    lookup("Regular:(Regular)", regular, sizeof regular);
    struct fm_block *found = NULL, *first = NULL;
    for (struct fm_block *b = fm_catalogue; b && !found; b = b->next) {
        if (!!(b->flags & LFFF_ENCODING) != enc)
            continue;
        uint32_t len;
        if (!strcmp_dot(want, b->name, &len))
            continue;
        const char *dw = strchr(want, '.'), *db = strchr(b->name, '.');
        if (!dw) {
            if (!first)
                first = b;
            if (b->flags & LFFF_DEFAULT)
                found = b;
            continue;
        }
        if (!db) {
            if (!strcmp_nocase(dw + 1, regular))
                found = b;
        } else if (!strcmp_nocase(dw + 1, db + 1)) {
            found = b;
        }
    }
    if (!found)
        found = first;
    if (!found) {
        ros_swi_fail(s, fm_err(enc ? FE_ENCNOTFOUND : FE_NOTFOUND, want, NULL));
        return;
    }
    char out[300];
    int n = snprintf(out, sizeof out, "\\%c%s", enc ? 'E' : 'F', found->id);
    if (found->name != found->id)
        snprintf(out + n, sizeof out - (size_t)n, "\\%c%u %s", enc ? 'e' : 'f', found->territory,
                 found->name);
    uint32_t len = (uint32_t)strlen(out) + 1;
    if (s->r[3]) {
        if (len > s->r[4]) {
            ros_swi_fail(s, fm_err(FE_BUFFOVERFLOW, NULL, NULL));
            return;
        }
        memcpy(ros_ptr(s->r[3]), out, len);
    }
    s->r[4] = len;
    s->r[2] = sel + used;
    s->v = 0;
}
