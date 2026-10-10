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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts03).
 */

/* files.c: a font's files.  This decides which files to use and reads
 * their headers (s/Fonts03's ScanFontDir, GetPixelsHeader and
 * GetMetricsHeader).
 *
 * A master's metrics come from IntMetrics<n>. Its outlines (the 1-bpp
 * leaf) come from Outlines<n>. Its 4-bpp bitmaps come from x90y45, or from
 * the file that x90y45 names.
 *
 * A slave uses b/f<xscale>x<yscale> bitmaps of exactly its size if they
 * exist. Otherwise it derives its data from the master's. Below FontMax1
 * it scales the master's 4-bpp bitmaps. Above that it uses the outlines.
 * These are either made into bitmaps in the cache, with subpixel positions
 * below FontMax4 and FontMax5, or drawn directly above FontMax2 and
 * FontMax3.
 *
 * Each decision is kept in the font's leaf, as the original keeps it in
 * its leafname, so each is made only once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fm.h"

/* ---- the filing system ------------------------------------------------------------------------ */

static uint32_t arena_string(const char *s)
{
    uint32_t a = ros_addr(ros_rma_alloc((uint32_t)strlen(s) + 1));
    strcpy(ros_ptr(a), s);
    return a;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

os_error *fm_file_type(const char *name, uint32_t *type, uint32_t *len)
{
    uint32_t a = arena_string(skip_spaces(name));
    uint32_t r[10] = { 17, a };
    os_error *e;
    fm_swi(XOS_File, r, &e);
    ros_rma_free(ros_ptr(a));
    *type = e ? 0 : r[0];
    *len = r[4];
    return e;
}

os_error *fm_load(const char *name, uint8_t **data, uint32_t *len)
{
    uint32_t type;
    os_error *e = fm_file_type(name, &type, len);
    if (e)
        return e;
    if (type != 1)
        *len = 0;
    if (*len > 0x7FFFFFFFu)
        return fm_err(FE_NOROOM, NULL, NULL);
    uint8_t *buf = ros_rma_alloc(*len + 1);
    if (!buf)
        return fm_err(FE_NOROOM, NULL, NULL);
    uint32_t a = arena_string(skip_spaces(name));
    uint32_t r[10] = { 16, a, ros_addr(buf), 0 };
    fm_swi(XOS_File, r, &e);
    ros_rma_free(ros_ptr(a));
    if (!e) {
        /* FM_PAD zero bytes follow the data. The readers of paths, kerns
         * and bitmaps stop at a zero. They may also look a few bytes beyond
         * the end. */
        *data = malloc(*len + FM_PAD);
        if (!*data) {
            e = fm_err(FE_NOROOM, NULL, NULL);
        } else {
            memcpy(*data, buf, *len);
            memset(*data + *len, 0, FM_PAD);
        }
    }
    ros_rma_free(buf);
    return e;
}

/* openfile, then read the header (OS_GBPB 3), then closefile.  *address is
 * set to the data's address if the file is in ResourceFS, as openpixels
 * notes it. */
static os_error *read_head(const char *name, uint32_t at, void *out, uint32_t n,
                           uint32_t *extent, uint32_t *address)
{
    uint32_t a = arena_string(skip_spaces(name)), buf = ros_addr(ros_rma_alloc(n ? n : 4));
    uint32_t r[10] = { 0x4F, a };
    os_error *e;
    fm_swi(XOS_Find, r, &e);
    ros_rma_free(ros_ptr(a));
    uint32_t h = r[0];
    if (!e && address) {
        uint32_t q[10] = { 21, h };
        os_error *e2;
        fm_swi(XOS_FSControl, q, &e2);
        *address = !e2 && (q[2] & 0xFF) == 46 ? q[1] : 0;
    }
    if (!e && extent) {
        uint32_t q[10] = { 2, h };
        fm_swi(XOS_Args, q, &e);
        *extent = q[2];
    }
    if (!e && n) {
        uint32_t q[10] = { 3, h, buf, n, at };
        fm_swi(XOS_GBPB, q, &e);
        if (!e && q[3])
            e = fm_err(FE_BADFONTFILE, NULL, NULL);        /* xos_gbpb transferred fewer bytes than asked */
        if (!e)
            memcpy(out, ros_ptr(buf), n);
    }
    if (h) {
        uint32_t c[10] = { 0, h };
        os_error *e2;
        fm_swi(XOS_Find, c, &e2);
    }
    ros_rma_free(ros_ptr(buf));
    return e;
}

static int exists(struct fm_font *f, const char *leaf, os_error **e)          /* testfile */
{
    char name[320];
    uint32_t type = 0, len;
    if (!(*e = fm_file_name(f, leaf, name, sizeof name)))
        *e = fm_file_type(name, &type, &len);
    return !*e && type == 1;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- GetMetricsHeader ------------------------------------------------------------------------------ */

os_error *fm_metrics_header(struct fm_font *f)
{
    char met[12], out[12], name[320];
    uint8_t h[12];
    uint32_t extent;
    os_error *e = fm_leafnames(f, met, out);
    if (!e)
        e = fm_file_name(f, met, name, sizeof name);
    if (!e)
        e = read_head(name, 40, h, sizeof h, &extent, &f->metaddress);
    if (e)
        return e;
    f->metoffset = 48;
    f->metsize = extent - 48;
    f->xsize = (int32_t)u32(h), f->ysize = (int32_t)u32(h + 4);
    f->metflags = h[10];
    f->nchars = h[8] | (uint32_t)h[11] << 8;
    return NULL;
}

/* ---- ScanFontDir ----------------------------------------------------------------------------------- */

static void set_file(struct fm_leaf *l, const char *leaf)
{
    l->type = LEAF_FILE;
    snprintf(l->name, sizeof l->name, "%s", leaf);
}

/* getxyscale: the size that the thresholds are measured against.  If a
 * transform block is in use, this is the block's own size. */
static void xy_scale(const struct fm_font *f, int32_t *xs, int32_t *ys)
{
    if (fm_trn) {
        *xs = fm_trn->xscale, *ys = fm_trn->yscale;
        return;
    }
    *xs = f->xscale * f->xmag;
    *ys = f->yscale * f->ymag;
}

/* getsubpixelflags_r3 */
static int subpixel(const struct fm_font *f, int32_t xs, int32_t ys)
{
    int r = 0;
    if (xs <= f->threshold[3])
        r |= PP_4XPOSNS;
    if (ys <= f->threshold[4])
        r |= PP_4YPOSNS;
    if (((fm_trn ? fm_trn->flags : f->flags) & 1) && ((r ^ r >> 1) & 1))
        r ^= PP_4XPOSNS | PP_4YPOSNS;
    return r;
}

/* convertfilename: <c><xscale/72>x<yscale/72>, at most ten characters */
static int bitmap_name(char c, int32_t xs, int32_t ys, char out[12])
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%c%ux%u", c, (unsigned)xs / 72, (unsigned)ys / 72);
    if (n > 10)
        return 0;
    memcpy(out, buf, (size_t)n + 1);
    return 1;
}

static os_error *no_data(struct fm_font *f)      /* err_fontdatanotfound */
{
    char name[41];
    size_t i = 0;
    for (; i < 40 && (uint8_t)f->name[i] >= 32; i++)
        name[i] = f->name[i];
    name[i] = 0;
    char out[256];
    fm_name_from_id(name, 0, out, sizeof out);
    return fm_err(FE_DATANOTFOUND2, out, NULL);
}

static os_error *scan_master(struct fm_font *f, struct fm_leaf *l)
{
    char met[12], out[12], name[320];
    os_error *e = fm_leafnames(f, met, out);
    int found = 0;
    if (!e && l == &f->leaf1) {
        if ((found = exists(f, out, &e)))
            set_file(l, out);
    } else if (!e) {
        uint32_t type, len;
        if (!(e = fm_file_name(f, "x90y45", name, sizeof name)) &&
            !(e = fm_file_type(name, &type, &len)) && type == 1) {
            if (len >= 255) {
                /* trybest_x90y45 with 'best' 1 accepts any size.  This is
                 * not yet done for x90y45 fonts. */
                found = 0;
            } else {
                uint8_t *leaf;
                if (!(e = fm_load(name, &leaf, &len))) {
                    char lf[12];
                    snprintf(lf, sizeof lf, "%s", (char *)leaf);
                    free(leaf);
                    if ((found = exists(f, lf, &e)))
                        set_file(l, lf);
                }
            }
        }
    }
    if (e) {
        l->type = LEAF_SCAN;
        return e;
    }
    if (!found) {
        l->type = LEAF_NONE;
        return no_data(f);
    }
    return NULL;
}

/* GetPixelsHeader_checknasty: "not found" is not an error here */
static os_error *master_header(struct fm_font *m, struct fm_leaf *l)
{
    if (l->type != LEAF_SCAN)
        return NULL;
    os_error *e = fm_pixels_header(m, l);
    if (e && (e->errnum & 0xFFFF) == FE_NOTFOUND)
        e = NULL;
    return e;
}

/* setskelthresh: decide whether skeleton lines are needed at this size.
 * It compares the master's threshold, in pixels, with the size. */
static void skel_thresh(struct fm_font *f, const struct fm_font *m, int32_t xs, int32_t ys)
{
    int32_t t = m->skelthresh * 72 * 16;
    uint8_t flags = 0;
    if (t >= 1 && t <= xs << 2 && t <= ys << 2) {
        flags |= 2;                                 /* sk4_dontbother */
        if (t <= xs && t <= ys)
            flags |= 1;                             /* sk1_dontbother */
    }
    f->skelthresh = flags;
}

static os_error *scan_slave(struct fm_font *f, struct fm_leaf *l, int bpp1)
{
    os_error *e = NULL;
    int32_t xs, ys;
    if (f->masterflag != MSF_RAMSCALED && !f->have_matrix && !fm_trn) {
        xy_scale(f, &xs, &ys);
        char name[12];
        if (bitmap_name(bpp1 ? 'b' : 'f', xs, ys, name)) {
            int found = exists(f, name, &e);
            if (e || found) {
                if (e)
                    l->type = LEAF_SCAN;
                else
                    set_file(l, name);
                return e;
            }
        }
        if (!bpp1) {
            char met[12], out[12];
            fm_leafnames(f, met, out);
            int outlines = exists(f, out, &e);
            if (!outlines || !subpixel(f, xs, ys)) {
                /* trybest_x90y45 for an exact match.  This is not yet done
                 * for x90y45 fonts. */
            }
        }
    }
    struct fm_font *m;
    if ((e = fm_font_ptr(f->masterfont, &m)) || (e = master_header(m, &m->leaf1)) ||
        (e = master_header(m, &m->leaf4))) {
        l->type = LEAF_SCAN;
        return e;
    }
    /* Get the render matrix before the thresholds, because a transformed
     * font's sizes come from it. */
    if (m->leaf1.type > LEAF_SCAN && !fm_trn && !f->render_ok && (e = fm_render_matrix(f))) {
        l->type = LEAF_SCAN;
        return e;
    }
    xy_scale(f, &xs, &ys);
    skel_thresh(f, m, xs, ys);
    int32_t h = ys;
    if (xs > ys << 1)
        h = xs >> 1;
    int type;
    /* A transformed font with a matrix of its own always comes from
     * outlines. */
    if (!bpp1 && !(f->have_matrix && fm_trn) && m->leaf4.type != LEAF_NONE &&
        !(subpixel(f, xs, ys) && m->leaf1.type != LEAF_NONE) &&
        (h <= f->threshold[0] || m->leaf1.type == LEAF_NONE)) {
        type = LEAF_4BPP;
    } else if (m->leaf1.type == LEAF_NONE) {
        type = LEAF_NONE;
    } else if (bpp1) {
        type = h > f->threshold[2] ? LEAF_DIRECT : LEAF_OUTLINES;
    } else if (m->leaf1.flags & PP_MONOCHROME) {
        type = LEAF_NONE;
    } else if (h > f->threshold[1] || h > f->threshold[2]) {
        type = LEAF_DIRECT;
    } else {
        type = LEAF_OUTLINES | subpixel(f, xs, ys);
    }
    l->type = (uint8_t)type;
    return type == LEAF_NONE ? no_data(f) : NULL;
}

/* ---- GetPixelsHeader --------------------------------------------------------------------------------- */

/* getnewheader: an Outlines, f9999x9999 or b9999x9999 file (fnew_) */
static os_error *new_header(struct fm_font *f, struct fm_leaf *l)
{
    char name[320];
    uint8_t h[62];
    os_error *e = fm_file_name(f, l->name, name, sizeof name);
    if (!e)
        e = read_head(name, 0, h, sizeof h, NULL, &l->address);
    if (e)
        return e;
    uint8_t version = h[5];
    if (version < 4 || version > 8 || memcmp(h, "FONT", 4)) {
        char n[41], out[256];
        size_t i = 0;
        for (; i < 40 && (uint8_t)f->name[i] >= 32; i++)
            n[i] = f->name[i];
        n[i] = 0;
        fm_name_from_id(n, 0, out, sizeof out);
        return fm_err(FE_BADFONTFILE2, out, NULL);
    }
    int32_t x0 = (int16_t)u16(h + 8), y0 = (int16_t)u16(h + 10);
    l->box[0] = x0, l->box[1] = y0;
    l->box[2] = x0 + u16(h + 12), l->box[3] = y0 + u16(h + 14);
    int outline = h[4] == 0;
    if (outline) {
        f->designsize = u16(h + 6);
        l->flags = (version >= 6 ? PP_DEPENDENCIES : 0) | (version >= 7 ? PP_FLAGSINFILE : 0);
        if (version <= 6)
            l->address = 0;             /* files of version 6 or older are not used in place from ROM */
    } else {
        l->flags = h[6];
        f->xscale = u16(h + 54) * u16(h + 56);             /* xsize * xres */
        f->yscale = u16(h + 58) * u16(h + 60);
    }
    uint32_t o0 = u32(h + 16), o1 = u32(h + 20), o2 = u32(h + 24), o3 = u32(h + 28);
    if (o0 > o1 || o1 > o2 || o2 > o3) {                     /* version 8 */
        l->pixoffstart = o0, l->nchunks = o1, l->nscaffolds = o2;
        l->flags |= (uint8_t)(o3 & (PP_16BITSCAFF | PP_MONOCHROME | PP_FILLNONZERO | PP_BIGTABLE));
    } else {
        l->pixoffstart = 16, l->nchunks = 8, l->nscaffolds = 256;
    }
    if (!outline)
        return NULL;
    f->scaffoldsize = l->flags & PP_BIGTABLE ? u32(h + 52) : u16(h + 52);
    f->skelthresh = 0;
    if (version >= 5) {
        uint32_t at = 52 + l->nscaffolds * (l->flags & PP_BIGTABLE ? 4 : 2);
        uint8_t sk;
        if ((e = read_head(name, at, &sk, 1, NULL, NULL)))
            return e;
        f->skelthresh = sk;
    }
    return NULL;
}

os_error *fm_pixels_header(struct fm_font *f, struct fm_leaf *l)
{
    return fm_pixels_header_bpp(f, l, l == &f->leaf1);
}

os_error *fm_pixels_header_bpp(struct fm_font *f, struct fm_leaf *l, int bpp1)
{
    if (l->type > LEAF_SCAN)
        return NULL;
    os_error *e = f->masterflag == MSF_MASTER ? scan_master(f, l) : scan_slave(f, l, bpp1);
    if (e)
        return e;
    if (l->type < LEAF_4BPP)
        return no_data(f);
    if (l->type == LEAF_FILE && l->name[0] == 'x')
        e = NULL;                       /* getoldheader is not yet done (x90y45 fonts) */
    else if (l->type == LEAF_FILE)
        e = new_header(f, l);
    else if (l->type >= LEAF_DIRECT)
        e = fm_outline_box(f, l);       /* masteroutlinebbox */
    /* masterbitmapbbox, for LEAF_4BPP, is not yet done (x90y45 fonts). */
    if (e)
        l->type = LEAF_SCAN;
    return e;
}
