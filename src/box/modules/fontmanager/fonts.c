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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts03, s.Fonts01).
 */

/* fonts.c: font handles.  This covers s/Fonts03's FindFont and DefineFont,
 * s/Fonts01's LoseFont, ReadDefn and ReadFontPrefix, and LookupFont.
 *
 * A handle names a font header. A header is either a master or a slave.
 * A master is a font at no particular size. Its files give its metrics
 * and its outlines or bitmaps. A slave is a size and resolution of a
 * master, perhaps with a matrix.
 *
 * Font_FindFont looks for a matching header, whether it is claimed or not.
 * Headers stay cached at usage 0. If there is none, it defines a new one
 * on the first free handle. If no handle is free, it uses the least
 * recently used unclaimed one.  A slave claims its master. It loses the
 * master again when its own usage falls to 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fm.h"

struct fm_font *fm_fonts[256];
uint32_t fm_currentfont = 1, fm_futurefont = 1;
int32_t fm_threshold[5] = { 16, 12, 24, 0, 0 };
uint32_t fm_maxcache;
static uint32_t clock;                  /* orders the claims on fonts */

os_error *fm_font_ptr(uint32_t h, struct fm_font **f)
{
    if (h == 0 || h >= 256)
        return fm_err(FE_BADFONTNUMBER, NULL, NULL);
    struct fm_font *p = fm_fonts[h];
    if (!p || !p->usage)
        return fm_err(FE_NOFONT, NULL, NULL);
    *f = p;
    return NULL;
}

/* deletefont: the header and all that hangs from it */
static void delete_font(uint32_t h)
{
    struct fm_font *f = fm_fonts[h];
    if (!f)
        return;
    fm_forget_maps(f);
    fm_forget_data(f);
    free(f->pathname), free(f->pathname2), free(f->charlist);
    free(f);
    fm_fonts[h] = NULL;
}

/* ---- names and files ------------------------------------------------------------------------ */

static const char *master_dir(struct fm_font *f, int outlines, os_error **e)
{
    *e = NULL;
    while (f->masterfont) {
        struct fm_font *m;
        if ((*e = fm_font_ptr(f->masterfont, &m)))
            return NULL;
        f = m;
    }
    return outlines && f->pathname2 ? f->pathname2 : f->pathname;
}

os_error *fm_file_name(struct fm_font *f, const char *leaf, char *out, size_t size)
{
    os_error *e;
    const char *dir = master_dir(f, leaf[0] == 'O', &e);
    if (!e)
        snprintf(out, size, "%s%s", dir ? dir : "", leaf);
    return e;
}

/* copy_r0r1_suffixR2: add the number, cutting the leafname to leave room
 * for it */
static void suffixed(const char *leaf, int32_t n, char out[12])
{
    if (n < 0) {
        strcpy(out, leaf);
        return;
    }
    char num[12];
    int k = snprintf(num, sizeof num, "%u", (unsigned)n);
    snprintf(out, 12, "%.*s%s", 10 - k, leaf, num);
}

os_error *fm_leafnames(struct fm_font *f, char metrics[12], char outlines[12])
{
    struct fm_font *m = f;
    os_error *e;
    if (f->masterfont && (e = fm_font_ptr(f->masterfont, &m)))
        return e;
    suffixed("IntMetrics", m->base, metrics);
    suffixed("Outlines", m->base, outlines);
    return NULL;
}

/* ---- the \M matrix ---------------------------------------------------------------------------- */

/* readinteger: reads a decimal number with an optional minus sign.
 * OS_ReadUnsigned must see it end at a space or control character. */
static int read_integer(const char **pp, int32_t *v)
{
    const char *p = *pp;
    uint8_t c;
    while ((c = (uint8_t)*p++) == ' ')
        ;
    if (c < ' ')
        return 0;
    int neg = c == '-';
    if (!neg)
        p--;
    uint32_t in = ros_addr(ros_rma_alloc(48));             /* (a longer number gives "Number too big") */
    size_t n = strnlen(p, 40);
    memcpy(ros_ptr(in), p, n);
    ((char *)ros_ptr(in))[n] = 0;
    uint32_t r[10] = { 0x8000000Au, in };
    os_error *e;
    fm_swi(XOS_ReadUnsigned, r, &e);
    ros_rma_free(ros_ptr(in));
    if (e)
        return 0;
    *pp = p + (r[1] - in);
    *v = neg ? -(int32_t)r[2] : (int32_t)r[2];
    return 1;
}

/* parsematrix_temp: if s has a \M field, set *have to 1 and fill in m[].
 * If not, set *have to 0.  A badly formed field gives an error. */
static os_error *parse_matrix(const char *s, int32_t m[6], int *have)
{
    *have = 0;
    const char *p = fm_find_field(s, 'M');
    if (!p)
        return NULL;
    for (int i = 0; i < 6; i++)
        if (!read_integer(&p, &m[i]))
            return fm_err(FE_BADMATRIX, NULL, NULL);
    uint8_t c;
    while ((c = (uint8_t)*p++) == ' ')
        ;
    if (c >= ' ' && c != '\\')
        return fm_err(FE_BADMATRIX, NULL, NULL);
    *have = 1;
    return NULL;
}

/* ---- MakePathName ------------------------------------------------------------------------------- */

/* getpathnamefromID: makes the path from the prefix and the catalogue's
 * own identifier, and returns the identifier as the name */
static os_error *path_from_id(const char *s, char **path, const char **id)
{
    struct fm_block *b;
    os_error *e = fm_find_block_field(s, 0, &b);
    if (e)
        return e;
    size_t n = strlen(b->prefix->prefix) + strlen(b->id) + 2;
    *path = malloc(n);
    snprintf(*path, n, "%s%s.", b->prefix->prefix, b->id);
    *id = b->id;
    return NULL;
}

/* makefontname: at most 39 characters, CR-padded */
static void set_name(struct fm_font *f, const char *id)
{
    size_t i = 0;
    for (; i < 39 && id[i] && (uint8_t)id[i] >= 33; i++)
        f->name[i] = id[i];
    memset(f->name + i, 13, 40 - i);
}

static os_error *make_path(struct fm_font *f, const char *s)
{
    const char *id;
    os_error *e = path_from_id(s, &f->pathname, &id);
    if (e)
        return e;
    set_name(f, id);
    if ((e = fm_base_encoding(f)))
        return e;
    char met[12], out[12], name[320];
    fm_leafnames(f, met, out);
    /* An Outlines file under 256 bytes names the font whose outlines to use. */
    uint32_t type, len;
    if ((e = fm_file_name(f, out, name, sizeof name)) || (e = fm_file_type(name, &type, &len)))
        return e;
    if (type != 1 || len > 255)
        return NULL;
    uint8_t *alias;
    if ((e = fm_load(name, &alias, &len)))
        return e;
    int have;
    if (!(e = parse_matrix((char *)alias, f->matrix, &have))) {
        f->have_matrix |= have;
        e = path_from_id((char *)alias, &f->pathname2, &id);
    }
    free(alias);
    return e;
}

/* ---- thresholds and resolutions ------------------------------------------------------------------ */

void fm_set_thresholds(struct fm_font *f)
{
    f->threshold[0] = (fm_threshold[0] << 4) * f->yres;
    f->threshold[1] = (fm_threshold[1] << 4) * f->yres;
    f->threshold[2] = (fm_threshold[2] << 4) * f->yres;
    f->threshold[4] = (fm_threshold[4] << 4) * f->yres;
    f->threshold[3] = (fm_threshold[3] << 4) * f->xres;
}

/* defaultres: 0 means the mode's pixels per inch */
static void default_res(uint32_t *xres, uint32_t *yres)
{
    if (!*xres)
        *xres = (uint32_t)(fm_divide(72000, fm_xscalefactor) >> fm_xeig);
    if (!*yres)
        *yres = (uint32_t)(fm_divide(72000, fm_yscalefactor) >> fm_yeig);
}

/* ---- FindFont -------------------------------------------------------------------------------------- */

static uint8_t want_enc[12];            /* encbuffer: the encoding FindFont is finding */

static os_error *find_font(const char *s, int32_t xs, int32_t ys, uint32_t *xres, uint32_t *yres,
                           uint32_t *h);

/* claimfont2: a slave that lost its master finds it again */
static os_error *claim(uint32_t h, const char *s)
{
    struct fm_font *f = fm_fonts[h];
    if (f->masterflag == MSF_NORMAL && !f->masterfont) {
        uint32_t m, xr = 0, yr = 0;
        os_error *e = find_font(s, -1, 0, &xr, &yr, &m);
        if (e)
            return e;
        f->masterfont = m;
    }
    f->usage++;
    f->age = ++clock;
    return NULL;
}

/* matchfont: finds a header with this name, encoding, size, resolution and
 * matrix.  If xs is negative it finds a master or a RAM-scaled font. */
static uint32_t match(const char *s, int32_t xs, int32_t ys, uint32_t xres, uint32_t yres,
                      const int32_t m[6], int have_m)
{
    const char *id = fm_find_field(s, 'F');
    for (uint32_t h = 1; h < 256; h++) {
        struct fm_font *f = fm_fonts[h];
        if (!f || fm_compare_id(f->name, id) || memcmp(f->encoding, want_enc, 12))
            continue;
        if (xs <= -1 && f->masterflag >= MSF_RAMSCALED)
            return h;
        if (f->xsize != xs || f->ysize != ys || f->xres != (int32_t)xres ||
            f->yres != (int32_t)yres)
            continue;
        if (!have_m && !f->have_matrix)
            return h;
        if (have_m && f->have_matrix && !memcmp(m, f->matrix, sizeof f->matrix))
            return h;
    }
    return 0;
}

/* DefineFont: the header for handle h */
static os_error *define_font(uint32_t h, const char *s, int32_t xs, int32_t ys, uint32_t xres,
                             uint32_t yres)
{
    delete_font(h);
    uint8_t c = (uint8_t)*s;
    if (c < 33 && c != 26)
        return NULL;                    /* a null name: only delete the font */
    struct fm_font *f = calloc(1, sizeof *f);
    fm_fonts[h] = f;
    f->xmag = f->ymag = 1;
    f->usage = 1;
    f->age = ++clock;
    f->leaf1.type = f->leaf4.type = LEAF_SCAN;
    memcpy(f->encoding, want_enc, 12);
    f->base = BASE_UNKNOWN;
    os_error *e;
    if (xs < 0) {
        f->masterflag = MSF_MASTER;
        if (!(e = make_path(f, s)))
            e = fm_metrics_header(f);
    } else {
        f->xsize = xs, f->ysize = ys, f->xres = (int32_t)xres, f->yres = (int32_t)yres;
        f->xscale = xs * (int32_t)xres, f->yscale = ys * (int32_t)yres;
        e = NULL;
        if (xres &&                                         /* getresmatrix */
            !(e = fm_res_value((int32_t)xres, &f->resxx)))
            e = fm_res_value((int32_t)yres, &f->resyy);
        fm_set_thresholds(f);
        /* A RAM-scaled font (26,<handle>,<chars>) cannot reach here.  Its
         * encoding is looked for as a font name first, and is not found. */
        uint32_t m, mxr = 0, myr = 0;
        if (!e)
            e = find_font(s, -1, 0, &mxr, &myr, &m);
        int have;
        if (!e && !(e = parse_matrix(s, f->matrix, &have))) {
            f->have_matrix = have;
            f->masterfont = m;
            struct fm_font *mf = fm_fonts[m];
            size_t i = 0;
            for (; i < 40 && (uint8_t)mf->name[i] >= 33; i++)
                f->name[i] = mf->name[i];
            char pad = !f->charlist && mf->masterflag == MSF_MASTER ? 13 : '*';
            for (; i < 40; i++)
                f->name[i] = pad, pad = 13;
            f->name[39] = 13;
            if (fm_pixels_header(f, &f->leaf4))
                e = fm_pixels_header(f, &f->leaf1);
        }
    }
    if (e) {
        fm_lose_font(h);
        delete_font(h);
        return e;
    }
    fm_currentfont = fm_futurefont = h;
    return NULL;
}

/* DefineFont for VDU 23,26: the encoding the last FindFont found */
os_error *fm_define_font(uint32_t h, const char *s, int32_t xs, int32_t ys, uint32_t xres,
                         uint32_t yres)
{
    return define_font(h, s, xs, ys, xres, yres);
}

/* IntFont_FindFont: want_enc must be set.  *xres and *yres are replaced by
 * defaults if they are 0. */
static os_error *find_font(const char *s, int32_t xs, int32_t ys, uint32_t *xres, uint32_t *yres,
                           uint32_t *out)
{
    os_error *e = fm_mode_vars();
    if (e)
        return e;
    default_res(xres, yres);
    if (!fm_find_field(s, 'F'))
        return fm_err(FE_FIELDNOTFOUND, "\\F", NULL);
    int32_t m[6];
    int have_m = 0;
    if (xs >= 0 && (e = parse_matrix(s, m, &have_m)))
        return e;
    uint32_t h = match(s, xs, ys, *xres, *yres, m, have_m);
    if (h) {
        if ((e = claim(h, s)))
            return e;
        fm_currentfont = fm_futurefont = h;
        *out = h;
        return NULL;
    }
    for (h = 1; h < 256 && fm_fonts[h]; h++)
        ;
    if (h == 256) {
        /* None is free.  Take the unclaimed font with the oldest age, which
         * is the time it was last claimed or lost. */
        uint32_t best = 0;
        for (uint32_t i = 1; i < 256; i++)
            if (!fm_fonts[i]->usage && (!best || fm_fonts[i]->age < fm_fonts[best]->age))
                best = i;
        if (!best)
            return fm_err(FE_NOHANDLES, NULL, NULL);
        h = best;
    }
    if ((e = define_font(h, s, xs, ys, *xres, *yres)))
        return e;
    *out = h;
    return NULL;
}

void fm_thunk_FindFont(struct ros_cpu *s)
{
    const char *str = ros_ptr(s->r[1]);
    os_error *e = fm_encoding_id(str, want_enc);
    uint32_t h;
    if (!e)
        e = find_font(str, (int32_t)s->r[2], (int32_t)s->r[3], &s->r[4], &s->r[5], &h);
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    s->r[0] = h;
    s->v = 0;
}

/* ---- LoseFont ------------------------------------------------------------------------------------ */

void fm_lose_font(uint32_t h)
{
    if (h == 0 || h >= 256 || !fm_fonts[h])
        return;
    struct fm_font *f = fm_fonts[h];
    if (f->usage <= 0 || --f->usage)
        return;
    f->age = ++clock;
    uint32_t m = f->masterfont;
    if (!m)
        return;
    f->masterfont = 0;                  /* found again when it is claimed again */
    if (f->masterflag == MSF_RAMSCALED)
        delete_font(h);
    fm_lose_font(m);
}

void fm_thunk_LoseFont(struct ros_cpu *s)
{
    fm_lose_font(s->r[0]);
    s->v = 0;
}

/* ---- ReadDefn ---------------------------------------------------------------------------------------- */

struct out { uint32_t at; int count; uint32_t n; };    /* R6: a buffer, or just a count so far */

static void put(struct out *o, const char *str)       /* appendR1toR6: appends up to a control character */
{
    for (; (uint8_t)*str >= 32; str++, o->n++)
        if (!o->count)
            ros_st8(o->at + o->n, (uint8_t)*str);
}

/* appendidandname: appends "\<Q><id>".  If the catalogue names it in a
 * territory, it then appends "\<q><territory> <name>". */
static void id_and_name(struct out *o, char q, const char *id, int encoding)
{
    char buf[16];
    snprintf(buf, sizeof buf, "\\%c", q);
    put(o, buf);
    put(o, id);
    struct fm_block *b;
    if (fm_find_block(id, encoding, &b) || (int32_t)b->territory <= 0)
        return;
    snprintf(buf, sizeof buf, "\\%c%u ", q | 0x20, (unsigned)b->territory);
    put(o, buf);
    put(o, b->name);
}

void fm_thunk_ReadDefn(struct ros_cpu *s)
{
    uint32_t h = s->r[0];
    struct fm_font *f = h && h < 256 ? fm_fonts[h] : NULL;
    if (!f) {
        ros_swi_fail(s, fm_err(FE_NOFONT, NULL, NULL));
        return;
    }
    if (s->r[3] == 0x4C4C5546u) {                       /* 'FULL' */
        struct out o = { s->r[1], s->r[1] < 0x8000, s->r[1] < 0x8000 ? s->r[1] : 0 };
        char name[41];
        memcpy(name, f->name, 40), name[40] = 13;
        id_and_name(&o, 'F', name, 0);
        if (memcmp(f->encoding, "\0\0\0\0", 4)) {
            char enc[13];
            memcpy(enc, f->encoding, 12), enc[12] = 0;
            id_and_name(&o, 'E', enc, 1);
        }
        if (f->have_matrix) {
            char m[128];
            int k = snprintf(m, sizeof m, "\\M");
            for (int i = 0; i < 6; i++)
                k += snprintf(m + k, sizeof m - (size_t)k, " %d", (int)f->matrix[i]);
            put(&o, m);
            s->r[3] = 0;
        }
        if (o.count) {
            s->r[2] = o.n + 1;
            s->r[6] = o.n;
            s->v = 0;
            return;
        }
        ros_st8(o.at + o.n, 0);
    } else {
        uint32_t i = 0;
        uint8_t c;
        do
            ros_st8(s->r[1] + i, c = (uint8_t)f->name[i < 40 ? i : 39]), i++;
        while (c >= 32);
    }
    if (f->masterflag == MSF_MASTER) {
        s->r[2] = s->r[3] = s->r[4] = s->r[5] = 0xFFFFFFFFu;
    } else {
        s->r[2] = (uint32_t)f->xsize, s->r[3] = (uint32_t)f->ysize;
        s->r[4] = (uint32_t)f->xres, s->r[5] = (uint32_t)f->yres;
    }
    s->r[6] = 0;                                        /* the age: always 0 */
    s->r[7] = (uint32_t)f->usage;
    s->v = 0;
}

/* ---- LookupFont ---------------------------------------------------------------------------------------- */

void fm_thunk_LookupFont(struct ros_cpu *s)
{
    if (s->r[2] || s->r[1]) {
        ros_swi_fail(s, fm_err(FE_RESERVED, NULL, NULL));
        return;
    }
    uint32_t h = s->r[0];
    struct fm_font *f = h && h < 256 ? fm_fonts[h] : NULL;
    struct fm_font *m = f && f->masterfont ? fm_fonts[f->masterfont] : NULL;
    if (!m) {
        ros_swi_fail(s, fm_err(FE_NOFONT, NULL, NULL));
        return;
    }
    uint32_t r2 = 0;
    if (m->leaf1.type > LEAF_SCAN) {
        if (m->leaf1.flags & PP_MONOCHROME)
            r2 |= 1u << 8;
        if (m->leaf1.flags & PP_FILLNONZERO)
            r2 |= 1u << 9;
        if (m->leaf1.address)
            r2 |= 2;
    }
    if (!(m->leaf1.type > LEAF_SCAN && m->leaf4.type > LEAF_SCAN) && m->leaf4.type > LEAF_SCAN) {
        if (m->leaf4.address)
            r2 |= 2;
        r2 |= 1;
    }
    s->r[2] = r2;
    s->v = 0;
}

/* ---- ReadFontPrefix ---------------------------------------------------------------------------------- */

void fm_thunk_ReadFontPrefix(struct ros_cpu *s)
{
    struct fm_font *f;
    char path[320];
    os_error *e = fm_font_ptr(s->r[0], &f);
    if (!e)
        e = fm_file_name(f, "", path, sizeof path);
    if (!e) {
        const char *p = path;
        while (*p == ' ')
            p++;
        uint32_t n = (uint32_t)strlen(p) + 1;
        if (n > s->r[2]) {
            memcpy(ros_ptr(s->r[1]), p, s->r[2]);
            e = fm_err(FE_BUFFOVERFLOW, NULL, NULL);
        } else {
            memcpy(ros_ptr(s->r[1]), p, n);
            s->r[1] += n - 1;
            s->r[2] -= n - 1;
        }
    }
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}
