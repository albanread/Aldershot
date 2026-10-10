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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts04, s.Font_Arith, s.Fonts01, s.Fonts02).
 */

/* metrics.c: a font's metrics and matrices, and the SWIs that read them.
 *
 * This is s/Fonts04's SetMetricsPtrs, CacheMetrics and GetTransform,
 * s/Font_Arith's render matrix, s/Fonts01's character bounding boxes and
 * s/Fonts02's CacheKerns. The SWIs are Font_CharBBox, Font_ReadInfo and
 * Font_ReadFontMetrics.
 *
 * A slave's metrics are its master's, scaled as they are read. A value in
 * the file is in 1/1000 em. It is multiplied by the slave's size and
 * divided by the master's size, both in 1/16 point. The result is rounded
 * down and is in millipoints. A font matrix (\M) or a paint matrix
 * transforms the metrics. The metrics matrix is the product of the two.
 *
 * Outlines are scaled by the bbox matrix, which goes from design units to
 * millipoints, and by the render matrix, which goes to pixels multiplied
 * by 512. Both bring in the font's size, its resolution, the master's
 * design size and the matrix that an alias's Outlines file gives. All of
 * these are the original's, computed in floating point (arith.c).
 *
 * The master's files are read once and whole, as the original's cache
 * holds them. Its metrics are read from offset 48. Its outlines supply
 * the character headers. A metrics file without bounding boxes (most of
 * the ROM's) takes its characters' boxes from their outlines.
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/swi.h"
#include "fm.h"

const uint8_t *fm_paintmatrix, *fm_oldpaintmatrix = FM_NOMATRIX;
int32_t fm_paintmatrixbuffer[6];
struct fm_metrics fm_met;
uint8_t fm_charflags;

#define BIGNUM 0x20000000

static int32_t s16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint32_t u16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t ror(uint32_t v, unsigned n) { return v >> n | v << (32 - n); }

static void words(const uint8_t *p, int32_t out[6])
{
    for (int i = 0; i < 6; i++)
        out[i] = (int32_t)u32(p + 4 * i);
}

/* setpaintmatrix: the unit matrix counts as no matrix. */
void fm_set_paint_matrix(const uint8_t *m)
{
    if (m) {
        int32_t v[6];
        words(m, v);
        if (v[0] == 0x10000 && !v[1] && !v[2] && v[3] == 0x10000 && !v[4] && !v[5])
            m = NULL;
    }
    fm_paintmatrix = m;
}

/* ---- the render matrix (Font_Arith: getrendermatrix) -------------------------------------------- */

/* The metrics matrix is F * P, the font matrix times the paint matrix.
 * The bbox matrix is the design-size scale, times the master's matrix,
 * times the size, times the metrics matrix. The render matrix is the bbox
 * matrix times the resolution. *xs, *ys and *swap are recomputed if there
 * is any matrix at all. */
static os_error *render_matrices(struct fm_font *f, struct fm_font *m, const uint8_t *paint,
                                 struct fm_mat *met, struct fm_mat *bbox, struct fm_mat *render,
                                 int32_t *xs, int32_t *ys, uint8_t *swap)
{
    struct fm_fpmat F, P, Met, S, B, D, R, T;
    const struct fm_fpmat *pF = NULL, *pMet;
    os_error *e;
    if (f->have_matrix) {
        fm_matrix_float(f->matrix, &F);
        pF = &F;
    }
    pMet = pF;
    if (paint) {
        int32_t v[6];
        words(paint, v);
        fm_matrix_float(v, &P);
        if ((e = fm_matrix_mul(pF, &P, &Met)))
            return e;
        pMet = &Met;
    }
    if (met && (e = fm_matrix_fix(pMet, met)))
        return e;
    S = fm_size_matrix(f->xsize, f->ysize);
    if (pMet && (e = fm_matrix_mul(&S, pMet, &S)))
        return e;
    B = S;
    if (m->have_matrix) {
        fm_matrix_float(m->matrix, &D);
        if ((e = fm_matrix_mul(&D, &S, &B)))
            return e;
    }
    if (m->designsize == 500) {
        fm_matrix_double(&B);
    } else if (m->designsize != 1000) {
        if ((e = fm_design_matrix(m->designsize, &D)) || (e = fm_matrix_mul(&D, &B, &B)))
            return e;
    }
    if ((e = fm_matrix_fix(&B, bbox)))
        return e;
    R = fm_res_matrix(f->resxx, f->resyy);
    if ((e = fm_matrix_mul(&B, &R, &T)) || (e = fm_matrix_fix(&T, render)))
        return e;
    *swap = 0;
    if (pF || paint)
        fm_transform_xyscale(render, m->designsize, xs, ys, swap);
    return NULL;
}

/* getnewrendermatrix, without a transform: the header's matrices. */
os_error *fm_render_matrix(struct fm_font *f)
{
    struct fm_font *m;
    os_error *e = fm_font_ptr(f->masterfont, &m);
    if (!e && m->leaf1.type <= LEAF_SCAN)
        e = fm_pixels_header(m, &m->leaf1);                 /* GetOutlinesHeader */
    if (e)
        return e;
    int32_t xs = f->xscale, ys = f->yscale;
    uint8_t swap;
    e = render_matrices(f, m, NULL, f->have_matrix ? &f->metmat : NULL, &f->bbox, &f->render, &xs,
                        &ys, &swap);
    f->render_ok = !e;
    if (!e) {
        f->xscale = xs, f->yscale = ys;
        f->flags = swap;
    }
    return e;
}

/* scaleoutlinebbox: a box in design units through the render matrix, in
 * pixels. The matrix result is multiplied by 512, so it is rounded and
 * shifted down by 9. */
static void scale_outline_box(const struct fm_mat *render, int32_t b[4])
{
    if (b[2] < b[0] || b[3] < b[1])
        b[0] = b[1] = b[2] = b[3] = 0;
    fm_transform_box(render, b);
    for (int i = 0; i < 4; i++)
        b[i] = (int32_t)((uint32_t)b[i] + 256) >> 9;
}

/* masteroutlinebbox: the master's box, through the render matrix, for
 * the slot l of the slave f. */
os_error *fm_outline_box(struct fm_font *f, struct fm_leaf *l)
{
    struct fm_font *m;
    os_error *e = fm_font_ptr(f->masterfont, &m);
    if (e)
        return e;
    memcpy(l->box, m->leaf1.box, sizeof l->box);
    scale_outline_box(fm_trn && l == &fm_trn->leaf ? &fm_trn->render : &f->render, l->box);
    return NULL;
}

/* ---- GetTransform ------------------------------------------------------------------------------ */

struct fm_trn *fm_trn;

/* getnewrendermatrix for a transform block */
static os_error *trn_matrices(struct fm_font *f, struct fm_trn *t)
{
    struct fm_font *m;
    os_error *e = fm_font_ptr(f->masterfont, &m);
    int32_t xs = f->xscale, ys = f->yscale;
    uint8_t swap = 0;
    if (!e)
        e = render_matrices(f, m, (const uint8_t *)t->pm, &t->met, &t->bbox, &t->render, &xs, &ys,
                            &swap);
    t->render_ok = !e;
    if (!e)
        t->xscale = xs, t->yscale = ys, t->flags = swap;
    return e;
}

static void free_trn(struct fm_trn *t)
{
    fm_delete_chunks(&t->leaf);
    free(t->leaf.data);
    free(t);
}

/* uncachetransforms: free the transform blocks and their data. */
void fm_forget_transforms(struct fm_font *f)
{
    while (f->trns) {
        struct fm_trn *t = f->trns;
        f->trns = t->next;
        if (fm_trn == t)
            fm_trn = NULL;
        free_trn(t);
    }
}

/* GetTransform: find the block for the paint matrix. The chain is searched
 * for a block with the same matrix and the same colour mode now in use
 * (anti-aliased or not). A block that is found is moved to the head of the
 * chain. Otherwise a new block is made. Its data is decided later, and its
 * chunk count is the master's. */
os_error *fm_get_transform(struct fm_font *f)
{
    if (!fm_paintmatrix) {
        fm_trn = NULL;
        if (f->have_matrix && !f->render_ok && f->masterflag != MSF_MASTER)
            fm_render_matrix(f);        /* the original's matrix would still be unset here */
        fm_met.matrix = f->have_matrix ? &f->metmat : NULL;
        return NULL;
    }
    int32_t pm[6];
    words(fm_paintmatrix, pm);
    int bpp1 = fm_current.acol == 0;
    struct fm_trn **pp = &f->trns, *t;
    for (; (t = *pp); pp = &t->next)
        if (t->bpp1 == bpp1 && !memcmp(t->pm, pm, sizeof pm))
            break;
    os_error *e;
    if (t) {
        *pp = t->next;
        t->next = f->trns, f->trns = t;
        fm_trn = t;
        if (!t->render_ok && (e = trn_matrices(f, t)))
            return e;
    } else {
        struct fm_font *m;
        if ((e = fm_font_ptr(f->masterfont, &m)) || (e = fm_pixels_header(m, &m->leaf1)))
            return e;
        t = calloc(1, sizeof *t);
        memcpy(t->pm, pm, sizeof pm);
        t->bpp1 = bpp1;
        t->leaf.type = LEAF_SCAN;
        t->leaf.nchunks = m->leaf1.nchunks;
        fm_trn = t;
        if ((e = trn_matrices(f, t))) {
            fm_trn = NULL;
            free_trn(t);
            fm_met.matrix = NULL;
            return e;
        }
        t->next = f->trns, f->trns = t;
    }
    fm_met.matrix = &t->met;
    return NULL;
}

/* ---- the metrics block (CacheMetrics, SetMetricsPtrs) ----------------------------------------- */

static void font_name(const struct fm_font *f, char out[41])
{
    size_t i = 0;
    for (; i < 40 && (uint8_t)f->name[i] >= 32; i++)
        out[i] = f->name[i];
    out[i] = 0;
}

/* Bad font file, naming the font. The metrics are the whole of a font's
 * description, so the metrics file is the one that is wrong. */
static os_error *bad_metrics(const struct fm_font *m)
{
    char n2[41], nm[256];
    font_name(m, n2);
    fm_name_from_id(n2, 0, nm, sizeof nm);
    return fm_err(FE_BADFONTFILE, nm, NULL);
}

static os_error *cache_metrics(struct fm_font *m)
{
    if (m->metrics)
        return NULL;
    char met[12], out[12], name[320];
    uint8_t *data;
    uint32_t len;
    os_error *e = fm_leafnames(m, met, out);
    if (!e)
        e = fm_file_name(m, met, name, sizeof name);
    if (!e)
        e = fm_load(name, &data, &len);
    if (e)
        return e;
    uint32_t n = len > 48 ? len - 48 : 0;
    m->metrics = calloc(1, n + 16);                 /* (zeros after the data, as FM_PAD) */
    if (!m->metrics) {
        free(data);
        return fm_err(FE_NOROOM, NULL, NULL);
    }
    memcpy(m->metrics, data + (len > 48 ? 48 : len), n);
    m->metlen = n;
    free(data);
    uint8_t version = m->metrics[1];
    int bad = version == 1 || version > 2 || (version == 0 && (m->metrics[2] || m->metrics[3]));
    if (bad) {
        free(m->metrics);
        m->metrics = NULL;
        return bad_metrics(m);
    }
    return NULL;
}

/* getmetricsblock: the block, and the scales for the current font */
static os_error *metrics_block(struct fm_font *f, struct fm_font **b)
{
    fm_met.xscale = fm_met.xfactor = f->xsize;
    fm_met.yscale = fm_met.yfactor = f->ysize;
    *b = f;
    if (f->masterfont) {
        struct fm_font *m;
        os_error *e = fm_font_ptr(f->masterfont, &m);
        if (!e && !m->metsize)
            e = fm_metrics_header(m);
        if (e)
            return e;
        fm_met.xfactor = m->xsize, fm_met.yfactor = m->ysize;
        *b = m;
    }
    return cache_metrics(*b);
}

os_error *fm_set_metrics(void)
{
    struct fm_font *f, *b;
    os_error *e = fm_font_ptr(fm_currentfont, &f);
    if (e)
        return e;
    if (fm_paintmatrix != fm_oldpaintmatrix) {
        fm_oldpaintmatrix = fm_paintmatrix;
        if ((e = fm_get_transform(f)))
            return e;
    }
    if ((e = metrics_block(f, &b)))
        return e;
    uint32_t n = b->nchars;
    const uint8_t *p = b->metrics + 4;                     /* met_chmap */
    const uint8_t *end = b->metrics + b->metlen;           /* the file's data ends here */
    uint8_t flags = b->metrics[2];
    fm_met.flags = flags;
    uint32_t mapsize = 256;
    if (flags & MET_MAPSIZED) {
        if (end - p < 2)
            return bad_metrics(b);
        mapsize = u16(p), p += 2;
    }
    if (end < p || (size_t)(end - p) < mapsize)
        return bad_metrics(b);
    fm_met.map = p;
    fm_met.mapsize = mapsize;
    fm_met.nchars = n;
    uint32_t block = n << 1;                                /* two bytes a character; never prescaled here */
    const uint8_t *q = p + mapsize;
    size_t arrays = (flags & MET_NOBBOXES ? 0 : 4 * (size_t)block) +
                    (flags & MET_NOXOFFSETS ? 0 : block) + (flags & MET_NOYOFFSETS ? 0 : block);
    if ((size_t)(end - q) < arrays)
        return bad_metrics(b);
    if (flags & MET_NOBBOXES) {
        memset(fm_met.bbox, 0, sizeof fm_met.bbox);
    } else {
        for (int i = 0; i < 4; i++)
            fm_met.bbox[i] = q, q += block;
    }
    fm_met.xoff = flags & MET_NOXOFFSETS ? NULL : q;
    if (!(flags & MET_NOXOFFSETS))
        q += block;
    fm_met.yoff = flags & MET_NOYOFFSETS ? NULL : q;
    if (!(flags & MET_NOYOFFSETS))
        q += block;
    fm_met.misc = fm_met.kerns = NULL;
    if (flags & MET_MOREDATA) {
        if (end - q < 6)
            return bad_metrics(b);
        const uint8_t *misc = q + u16(q), *kerns = q + u16(q + 2);
        /* the misc area is 24 bytes, or none if the kerns start where it does */
        if (misc > kerns || kerns > end || (kerns != misc && kerns - misc < 24))
            return bad_metrics(b);
        fm_met.misc = misc;
        fm_met.kerns = kerns;
        if (end - misc < 10)                                /* scan.c reads a width at offset 8 */
            fm_met.misc = NULL;
    }
    fm_met.valid = 1;
    return NULL;
}

static int32_t scale(int32_t v, int32_t mul, int32_t div)
{
    int32_t t = (int32_t)((uint32_t)v * (uint32_t)mul);
    return div == 16 ? t >> 4 : fm_divide(t, div);
}

void fm_scale_width(int32_t *x, int32_t *y)
{
    *x = scale(*x, fm_met.xscale, fm_met.xfactor);
    if (*y)
        *y = scale(*y, fm_met.yscale, fm_met.yfactor);
}

int32_t fm_scale_x(int32_t v) { return scale(v, fm_met.xscale, fm_met.xfactor); }
int32_t fm_scale_y(int32_t v) { return scale(v, fm_met.yscale, fm_met.yfactor); }

/* ---- chunks and character headers ------------------------------------------------------------ */

os_error *fm_load_chunk(struct fm_font *f, struct fm_leaf *l, uint32_t c, struct fm_chunkp *ck)
{
    ck->index = NULL, ck->flags = 0, ck->end = NULL;
    if (!l->data) {
        char name[320];
        os_error *e = fm_file_name(f, l->name, name, sizeof name);
        if (!e)
            e = fm_load(name, &l->data, &l->len);
        if (e)
            return e;
    }
    /* The table must lie inside the file. The sum is done in 64 bits so that
     * it cannot wrap. */
    if (c >= l->nchunks || (uint64_t)l->pixoffstart + 4 * ((uint64_t)c + 2) > l->len)
        return NULL;
    uint32_t a = u32(l->data + l->pixoffstart + 4 * c), b = u32(l->data + l->pixoffstart + 4 * c + 4);
    if (a >= b || b > l->len)
        return NULL;
    const uint8_t *p = l->data + a;
    ck->end = l->data + l->len;
    if (l->flags & PP_FLAGSINFILE) {
        if (b - a < 4)
            return NULL;
        ck->flags = u32(p);
        p += 4;
    } else {
        ck->flags = l->flags;
    }
    if ((size_t)(ck->end - p) < 128)                    /* the index of 32 characters */
        return NULL;
    ck->index = p;
    return NULL;
}

const uint8_t *fm_char_in(const struct fm_chunkp *ck, uint32_t code)
{
    if (!ck->index)
        return NULL;
    uint32_t off = u32(ck->index + 4 * (code & 31));
    return off && off < (size_t)(ck->end - ck->index) ? ck->index + off : NULL;
}

/* readbbox: x, y, w, h after the flag byte */
const uint8_t *fm_read_bbox(const uint8_t *p, uint8_t flags, int32_t b[4])
{
    if (!(flags & CHF_12BIT)) {
        b[0] = (int8_t)p[0], b[1] = (int8_t)p[1], b[2] = p[2], b[3] = p[3];
        return p + 4;
    }
    b[0] = (int32_t)((uint32_t)p[0] << 20 | (uint32_t)p[1] << 28) >> 20;
    b[1] = (int32_t)((uint32_t)p[2] << 24 | (uint32_t)p[1] << 16) >> 20;
    b[2] = (int32_t)(p[3] | (p[4] & 15) << 8);
    b[3] = (int32_t)(p[4] >> 4 | p[5] << 4);
    return p + 6;
}

/* getbbox_unscaled_uncomp: the box of the character itself, without any
 * components. */
static void bbox_uncomp(const uint8_t *p, int32_t b[4])
{
    uint8_t flags = p[0];
    fm_charflags = flags;
    if ((flags & CHF_OUTLINES) && (flags & (CHF_COMPOSITE1 | CHF_COMPOSITE2))) {
        b[0] = b[1] = BIGNUM;
        b[2] = b[3] = -2 * BIGNUM;
        return;
    }
    fm_read_bbox(p + 1, flags, b);                            /* x and y magnification are 1: never magnified here */
}

/* getbbox_unscaled_R1: a component, from the master's outlines */
static os_error *bbox_component(struct fm_font *m, uint32_t code, int32_t b[4])
{
    struct fm_chunkp ck;
    os_error *e = fm_load_chunk(m, &m->leaf1, code >> 5, &ck);
    if (e)
        return e;
    const uint8_t *p = fm_char_in(&ck, code);
    if (p) {
        bbox_uncomp(p, b);
    } else {
        b[0] = b[1] = BIGNUM;
        b[2] = b[3] = -2 * BIGNUM;
    }
    return NULL;
}

/* getbbox_unscaled: x, y, w, h in design units. A composite's box is made
 * from its base and its accent. The accent's offset is read as a 12-bit
 * pair, whatever the character's coordinates are. */
static os_error *bbox_unscaled(struct fm_font *m, const uint8_t *p, int32_t b[4])
{
    uint8_t flags = p[0];
    if (!(flags & CHF_OUTLINES) || !(flags & (CHF_COMPOSITE1 | CHF_COMPOSITE2))) {
        bbox_uncomp(p, b);
        return NULL;
    }
    p++;
    os_error *e;
    int32_t base[4];
    if (flags & CHF_COMPOSITE1) {
        uint32_t c = *p++;
        if (flags & CHF_16BITCODES)
            c |= (uint32_t)*p++ << 8;
        if ((e = bbox_component(m, c, b)))
            return e;
        b[2] = (int32_t)((uint32_t)b[0] + (uint32_t)b[2]);
        b[3] = (int32_t)((uint32_t)b[1] + (uint32_t)b[3]);
    }
    if (flags & CHF_COMPOSITE2) {
        if (flags & CHF_COMPOSITE1)
            memcpy(base, b, sizeof base);
        uint32_t c = *p++;
        if (flags & CHF_16BITCODES)
            c |= (uint32_t)*p++ << 8;
        if ((e = bbox_component(m, c, b)))
            return e;
        int32_t ox = (int32_t)((uint32_t)p[0] << 20 | (uint32_t)p[1] << 28) >> 20;
        int32_t oy = (int32_t)((uint32_t)p[2] << 24 | (uint32_t)p[1] << 16) >> 20;
        b[0] = (int32_t)((uint32_t)b[0] + (uint32_t)ox);
        b[1] = (int32_t)((uint32_t)b[1] + (uint32_t)oy);
        b[2] = (int32_t)((uint32_t)b[0] + (uint32_t)b[2]);
        b[3] = (int32_t)((uint32_t)b[1] + (uint32_t)b[3]);
        if (flags & CHF_COMPOSITE1) {
            if (b[0] > base[0]) b[0] = base[0];
            if (b[1] > base[1]) b[1] = base[1];
            if (b[2] < base[2]) b[2] = base[2];
            if (b[3] < base[3]) b[3] = base[3];
        }
    }
    b[2] = (int32_t)((uint32_t)b[2] - (uint32_t)b[0]);
    b[3] = (int32_t)((uint32_t)b[3] - (uint32_t)b[1]);
    fm_charflags = flags;
    return NULL;
}

/* getbbox: a character's pixel box as x, y, w, h. An outline character's
 * box goes through the render matrix and gets a margin of one pixel, or
 * two when the last painting was anti-aliased. The chf_1bpp bit of
 * charflags says which. */
os_error *fm_bbox_pixels(struct fm_font *m, const struct fm_mat *render, const uint8_t *p,
                             int32_t b[4])
{
    if (!(p[0] & CHF_OUTLINES)) {
        fm_charflags = p[0];
        fm_read_bbox(p + 1, p[0], b);
        return NULL;
    }
    uint8_t before = fm_charflags;
    os_error *e = bbox_unscaled(m, p, b);
    if (e)
        return e;
    fm_charflags = (uint8_t)((fm_charflags & ~CHF_1BPP) | (before & CHF_1BPP));
    int32_t margin = before & CHF_1BPP ? 1 : 2;
    b[2] = (int32_t)((uint32_t)b[0] + (uint32_t)b[2]);
    b[3] = (int32_t)((uint32_t)b[1] + (uint32_t)b[3]);
    scale_outline_box(render, b);
    b[0] -= margin, b[1] -= margin, b[2] += margin, b[3] += margin;
    b[2] -= b[0], b[3] -= b[1];
    return NULL;
}

/* getoutlines_metricsbbox */
os_error *fm_outline_metrics_bbox(int32_t g, int32_t box[4])
{
    struct fm_font *f, *m;
    os_error *e = fm_font_ptr(fm_currentfont, &f);
    if (!e && !fm_trn && !f->render_ok)
        e = fm_render_matrix(f);                            /* bboxmetrics_getmatrix */
    if (!e)
        e = fm_font_ptr(f->masterfont, &m);
    struct fm_chunkp ck;
    if (!e)
        e = fm_load_chunk(m, &m->leaf1, (uint32_t)g >> 5, &ck);
    if (e)
        return e;
    const uint8_t *p = fm_char_in(&ck, (uint32_t)g);
    box[0] = box[1] = box[2] = box[3] = 0;
    if (p) {
        if ((e = bbox_unscaled(m, p, box)))
            return e;
    }
    box[2] = (int32_t)((uint32_t)box[0] + (uint32_t)box[2]);
    box[3] = (int32_t)((uint32_t)box[1] + (uint32_t)box[3]);
    fm_transform_box(fm_trn ? &fm_trn->bbox : &f->bbox, box);
    return NULL;
}

/* ---- kerns (CacheKerns, getkernpair) ---------------------------------------------------------- */

#define KERN_FLAGS (257 * 4)
#define KERN_DATA (KERN_FLAGS + 12)
#define FLG_SHORTKERNS 0x80000000u

static uint32_t kern_hash(uint32_t l, uint32_t r)
{
    uint32_t h = l ^ ror(r, 4);
    h ^= ror(h, 16);
    h ^= ror(h, 8);
    return h & 0xFF;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

os_error *fm_kerns(struct fm_font *m, const uint8_t **blk)
{
    *blk = NULL;
    if (m->kernsize == 1)
        return NULL;
    if (m->kernsize) {
        *blk = m->kerns;
        return NULL;
    }
    os_error *e = cache_metrics(m);
    if (e)
        return e;
    uint32_t n = m->nchars, flags = m->metrics[2];
    const uint8_t *p = m->metrics + 4;
    if (!(flags & MET_MOREDATA)) {
        m->kernsize = 1;
        return NULL;
    }
    const uint8_t *end = m->metrics + m->metlen;
    /* the layout that fm_set_metrics checks. Kerns that do not fit count as
     * none. */
    size_t front = (flags & MET_MAPSIZED ? 2 + (size_t)u16(p) : 256) +
                   (flags & MET_NOBBOXES ? 0 : (size_t)n << 3) +
                   (flags & MET_NOXOFFSETS ? 0 : (size_t)n << 1) +
                   (flags & MET_NOYOFFSETS ? 0 : (size_t)n << 1);
    if (end < p || (size_t)(end - p) < front + 6) {
        m->kernsize = 1;
        return NULL;
    }
    p += front;
    uint32_t a = u16(p + 2), z = u16(p + 4);
    if (a >= z || a >= (size_t)(end - p)) {
        m->kernsize = 1;
        return NULL;
    }
    const uint8_t *k = p + a;
    int wide = (flags & MET_16BITKERNS) != 0;
    int step = (flags & MET_NOXOFFSETS ? 0 : 2) + (flags & MET_NOYOFFSETS ? 0 : 2);
    /* Count the pairs and find the highest code. */
    uint32_t pairs = 0, max = 0;
    const uint8_t *q = k;
    for (;;) {
        if (q >= end)                   /* a table with no end stops at the end of the data */
            break;
        uint32_t l = *q++;
        if (wide)
            l |= (uint32_t)*q++ << 8;
        if (!l)
            break;
        for (;;) {
            if (q >= end)
                break;
            uint32_t r = *q++;
            if (wide)
                r |= (uint32_t)*q++ << 8;
            if (!r)
                break;
            if (l > max) max = l;
            if (r > max) max = r;
            pairs++;
            q += step;
        }
    }
    uint32_t size = pairs << 3;
    if (max <= 255 && (flags & (MET_NOXOFFSETS | MET_NOYOFFSETS))) {
        flags |= FLG_SHORTKERNS;
        size = pairs << 2;
    }
    /* The pairs, hashed. Each bucket is a list with the latest pair at its
     * head. */
    struct pair { uint32_t lr, xy; int next; } *list = malloc((pairs + 1) * sizeof *list);
    if (!list)
        return fm_err(FE_NOROOM, NULL, NULL);
    int head[256];
    for (int i = 0; i < 256; i++)
        head[i] = -1;
    int used = 0;
    q = k;
    for (;;) {
        if (q >= end)
            break;
        uint32_t l = *q++;
        if (wide)
            l |= (uint32_t)*q++ << 8;
        if (!l)
            break;
        for (;;) {
            if (q >= end)
                break;
            uint32_t r = *q++;
            if (wide)
                r |= (uint32_t)*q++ << 8;
            if (!r)
                break;
            uint32_t x = 0, y = 0;
            if (!(flags & MET_NOXOFFSETS))
                x = u16(q), q += 2;
            if (!(flags & MET_NOYOFFSETS))
                y = u16(q), q += 2;
            uint32_t h = kern_hash(l, r);
            list[used] = (struct pair){ l | r << 16, x | y << 16, head[h] };
            head[h] = used++;
        }
    }
    uint8_t *b = calloc(1, KERN_DATA + size);
    if (!b) {
        free(list);
        return fm_err(FE_NOROOM, NULL, NULL);
    }
    uint32_t out = KERN_DATA;
    for (int h = 0; h < 256; h++) {
        put32(b + 4 * h, out);
        for (int i = head[h]; i >= 0; i = list[i].next) {
            if (!(flags & FLG_SHORTKERNS)) {
                put32(b + out, list[i].lr), put32(b + out + 4, list[i].xy);
                out += 8;
            } else {
                uint32_t lr = list[i].lr, xy = list[i].xy;
                uint32_t w = (lr & 0xFF) | (lr >> 16 & 0xFF) << 8;
                if (flags & MET_NOXOFFSETS)
                    xy >>= 16;
                put32(b + out, w | xy << 16);
                out += 4;
            }
        }
    }
    put32(b + 4 * 256, out);
    put32(b + KERN_FLAGS, flags);
    free(list);
    m->kerns = b;
    m->kernsize = KERN_DATA + size;
    *blk = b;
    return NULL;
}

/* getkernpair: returns 1, with the unscaled offsets, if the pair is kerned. */
int fm_kern_pair(const uint8_t *blk, uint32_t l, uint32_t r, int32_t *kx, int32_t *ky)
{
    uint32_t flags = u32(blk + KERN_FLAGS);
    if ((flags & FLG_SHORTKERNS) && (l >= 256 || r >= 256))
        return 0;
    uint32_t h = kern_hash(l, r), a = u32(blk + 4 * h), z = u32(blk + 4 * h + 4);
    if (a >= z)
        return 0;
    if (flags & FLG_SHORTKERNS) {
        uint32_t key = (l | r << 8) << 16;
        for (; a < z; a += 4) {
            uint32_t w = u32(blk + a);
            if (key == w << 16) {
                if (flags & MET_NOXOFFSETS)
                    *kx = 0, *ky = (int32_t)w >> 16;
                else
                    *kx = (int32_t)w >> 16, *ky = 0;
                return 1;
            }
        }
        return 0;
    }
    uint32_t key = l | r << 16;
    for (; a < z; a += 8)
        if (u32(blk + a) == key) {
            uint32_t w = u32(blk + a + 4);
            *kx = (int16_t)w, *ky = (int32_t)w >> 16;
            return 1;
        }
    return 0;
}

/* ---- Font_CharBBox ------------------------------------------------------------------------------- */

static void box_to_os(int32_t b[4])                        /* scalepixtoOS */
{
    b[0] = (int32_t)((uint32_t)b[0] << fm_xeig), b[2] = (int32_t)((uint32_t)b[2] << fm_xeig);
    b[1] = (int32_t)((uint32_t)b[1] << fm_yeig), b[3] = (int32_t)((uint32_t)b[3] << fm_yeig);
}

/* A character's box from the metrics, in millipoints. */
static os_error *bbox_metrics(int32_t g, int32_t b[4])
{
    os_error *e = fm_set_metrics();
    if (e)
        return e;
    if (!fm_met.bbox[0])
        return fm_outline_metrics_bbox(g, b);
    uint32_t idx = (uint32_t)g;
    if (fm_met.mapsize) {
        if (idx >= fm_met.mapsize) {
            b[0] = b[1] = b[2] = b[3] = 0;
            return NULL;
        }
        idx = fm_met.map[idx];
    }
    if (idx >= fm_met.nchars) {                     /* (no such character in the metrics) */
        b[0] = b[1] = b[2] = b[3] = 0;
        return NULL;
    }
    int32_t x1 = s16(fm_met.bbox[2] + 2 * idx), y1 = s16(fm_met.bbox[3] + 2 * idx);
    fm_scale_width(&x1, &y1);
    int32_t x0 = s16(fm_met.bbox[0] + 2 * idx), y0 = s16(fm_met.bbox[1] + 2 * idx);
    fm_scale_width(&x0, &y0);
    b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1;
    if (fm_met.matrix)
        fm_transform_box(fm_met.matrix, b);
    return NULL;
}

/* a character's box in pixels, x0 y0 inclusive, x1 y1 exclusive */
static os_error *bbox_pixel(int32_t g, int32_t b[4])
{
    struct fm_font *f, *m;
    os_error *e = fm_font_ptr(fm_currentfont, &f);
    if (e)
        return e;
    struct fm_leaf *l = fm_current.acol == 0 ? &f->leaf1 : &f->leaf4;
    if (l->type == LEAF_SCAN)
        fm_pixels_header(f, l);                             /* errors ignored for now */
    if (l->type == LEAF_NONE)
        l = l == &f->leaf1 ? &f->leaf4 : &f->leaf1;
    if (l->type == LEAF_SCAN)
        fm_pixels_header(f, l);
    if (l->type == LEAF_NONE || l->type == LEAF_SCAN) {
        /* (the original passes R0, a font handle, as the error pointer. This
         * makes a real error, "No suitable font data for <name>".) */
        char n[41], nm[256];
        font_name(f, n);
        fm_name_from_id(n, 0, nm, sizeof nm);
        return fm_err(FE_DATANOTFOUND2, nm, NULL);
    }
    struct fm_chunkp ck;
    const uint8_t *p;
    if (l->type >= LEAF_FILE) {                             /* the slave's own file */
        if ((e = fm_load_chunk(f, l, (uint32_t)g >> 5, &ck)))
            return e;
        p = fm_char_in(&ck, (uint32_t)g);
        m = f;
    } else if (l->type == LEAF_4BPP) {
        return fm_err(FE_NOSUCHSWI, NULL, NULL);            /* x90y45 fonts are not supported yet */
    } else {                                                /* the master's outlines */
        if ((e = fm_font_ptr(f->masterfont, &m)))
            return e;
        if (!f->render_ok && (e = fm_render_matrix(f)))
            return e;
        if ((e = fm_load_chunk(m, &m->leaf1, (uint32_t)g >> 5, &ck)))
            return e;
        p = fm_char_in(&ck, (uint32_t)g);
    }
    if (!p) {                                               /* getoutlinebbox: no such character, so an empty box */
        b[0] = b[1] = b[2] = b[3] = 0;
        return NULL;
    }
    if ((e = fm_bbox_pixels(m, &f->render, p, b)))
        return e;
    b[2] = (int32_t)((uint32_t)b[0] + (uint32_t)b[2]);
    b[3] = (int32_t)((uint32_t)b[1] + (uint32_t)b[3]);
    if (ck.flags & PP_4XPOSNS)
        b[2]++;
    if (ck.flags & PP_4YPOSNS)
        b[3]++;
    return NULL;
}

static os_error *char_bbox(uint32_t h, uint32_t code, uint32_t flags, int32_t b[4])
{
    fm_paintmatrix = NULL;
    fm_trn = NULL;
    fm_oldpaintmatrix = FM_NOMATRIX;
    if (h >= 256)
        return fm_err(FE_BADFONTNUMBER, NULL, NULL);
    if (code >= 0x80000000u)
        return fm_err(FE_BADCHARCODE, NULL, NULL);
    if (flags & ~0x10u)
        return fm_err(FE_RESERVED, NULL, NULL);
    fm_currentfont = h;
    struct fm_font *f;
    int32_t g;
    os_error *e = fm_font_ptr(h, &f);
    if (!e)
        e = fm_map_char(f, (int32_t)code, &g);
    if (e)
        return e;
    if (g == -1) {
        b[0] = b[1] = b[2] = b[3] = 0;
        return NULL;
    }
    if (!(flags & 0x10))
        return bbox_metrics(g, b);
    if ((e = bbox_pixel(g, b)))
        return e;
    box_to_os(b);
    return NULL;
}

void fm_thunk_CharBBox(struct ros_cpu *s)
{
    uint32_t cur = fm_currentfont;
    int32_t b[4];
    os_error *e = char_bbox(s->r[0], s->r[1], s->r[2], b);
    fm_currentfont = cur;
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    for (int i = 0; i < 4; i++)
        s->r[1 + i] = (uint32_t)b[i];
    s->v = 0;
}

/* ---- Font_ReadInfo --------------------------------------------------------------------------------- */

void fm_thunk_ReadInfo(struct ros_cpu *s)
{
    struct fm_font *f;
    os_error *e = fm_mode_vars();
    if (!e)
        e = fm_font_ptr(s->r[0], &f);
    for (int tries = 0; !e; tries++) {
        struct fm_leaf *l = f->leaf4.type > LEAF_SCAN ? &f->leaf4
                          : f->leaf1.type > LEAF_SCAN ? &f->leaf1 : NULL;
        if (l) {
            int32_t b[4];
            memcpy(b, l->box, sizeof b);
            box_to_os(b);
            for (int i = 0; i < 4; i++)
                s->r[1 + i] = (uint32_t)b[i];
            s->v = 0;
            return;
        }
        if (tries == 2) {               /* the original would loop for ever */
            char n[41], nm[256];
            font_name(f, n);
            fm_name_from_id(n, 0, nm, sizeof nm);
            e = fm_err(FE_DATANOTFOUND2, nm, NULL);
            break;
        }
        fm_paintmatrix = NULL;
        fm_trn = NULL;
        fm_oldpaintmatrix = FM_NOMATRIX;
        if ((e = fm_pixels_header(f, &f->leaf4)))
            e = fm_pixels_header(f, &f->leaf1);
    }
    ros_swi_fail(s, e);
}

/* ---- Font_ReadFontMetrics ------------------------------------------------------------------------ */

/* readfontmetrics_setparams */
static os_error *rfm_params(uint32_t h)
{
    struct fm_font *f;
    os_error *e = fm_font_ptr(h, &f);
    if (e)
        return e;
    if (f->masterflag != MSF_MASTER && f->have_matrix)
        return fm_err(FE_BADREADMETRICS, NULL, NULL);
    fm_currentfont = h;
    fm_met.valid = 0;
    return fm_set_metrics();
}

/* ConvertKernsToOldForm: write the kerns of codes 0-255 to out, hashed as
 * the old format did. If out is 0 they are only counted. *size is the
 * size of the result. */
static os_error *old_kerns(struct fm_font *f, struct fm_font *m, const uint8_t *blk, uint32_t out,
                           uint32_t *size)
{
    uint32_t flags = u32(blk + KERN_FLAGS);
    if (!out && m->oldkernsize) {
        *size = m->oldkernsize;
        return NULL;
    }
    uint32_t at = (out ? out : 0) + 257 * 4 + 12;          /* kern_data - kern_index */
    if (out) {
        ros_st32(out, at - out);
        ros_st32(out + 257 * 4, flags & ~FLG_SHORTKERNS);
        ros_st32(out + 257 * 4 + 4, 0);
        ros_st32(out + 257 * 4 + 8, 0);
    }
    for (uint32_t h = 0; h < 256; h++) {
        for (int32_t l = 255; l >= 0; l--) {
            uint32_t t = h ^ (uint32_t)l, rc = ((t << 4) & 0xFF) | t >> 4;
            int32_t gr, gl;
            os_error *e = fm_map_char(f, (int32_t)rc, &gr);
            if (!e)
                e = fm_map_char(f, l, &gl);
            if (e)
                return e;
            int32_t kx, ky;
            if ((gl | gr) < 0 || !fm_kern_pair(blk, (uint32_t)gl, (uint32_t)gr, &kx, &ky))
                continue;
            fm_scale_width(&kx, &ky);
            if (out) {
                ros_st32(at, (uint32_t)l);
                at += 4;
                if (!(flags & MET_NOXOFFSETS))
                    ros_st32(at, (uint32_t)kx), at += 4;
                if (!(flags & MET_NOYOFFSETS))
                    ros_st32(at, (uint32_t)ky), at += 4;
            } else {
                at += 4 + (flags & MET_NOXOFFSETS ? 0 : 4) + (flags & MET_NOYOFFSETS ? 0 : 4);
            }
        }
        if (out)
            ros_st32(out + 4 * (h + 1), at - out);
    }
    *size = at - (out ? out : 0);
    m->oldkernsize = *size;
    return NULL;
}

static os_error *read_font_metrics(struct ros_cpu *s)
{
    uint32_t r[8];
    memcpy(r, s->r, sizeof r);
    if (r[7])
        return fm_err(FE_RESERVED, NULL, NULL);
    fm_paintmatrix = NULL;
    fm_trn = NULL;
    fm_oldpaintmatrix = FM_NOMATRIX;
    os_error *e = rfm_params(r[0]);
    if (e)
        return e;
    uint8_t flags = fm_met.flags;
    struct fm_font *f, *m;
    if ((e = fm_font_ptr(fm_currentfont, &f)))
        return e;
    /* R1: the boxes of codes 0-255 */
    if (r[1]) {
        uint32_t at = r[1];
        for (uint32_t c = 0; c < 256; c++) {
            int32_t b[4];
            uint32_t cur = fm_currentfont;
            e = char_bbox(fm_currentfont, c, 0, b);
            fm_currentfont = cur;
            if (e)
                return e;
            for (int i = 0; i < 4; i++, at += 4)
                ros_st32(at, (uint32_t)b[i]);
        }
        if ((e = rfm_params(fm_currentfont)))
            return e;
    }
    s->r[1] = 16 * 256;
    /* R2, R3: the advances */
    for (int axis = 0; axis < 2; axis++) {
        uint32_t at = r[2 + axis];
        const uint8_t *arr = axis ? fm_met.yoff : fm_met.xoff;
        uint8_t none = axis ? MET_NOYOFFSETS : MET_NOXOFFSETS;
        if (at && !(flags & none)) {
            for (uint32_t c = 0; c < 256; c++, at += 4) {
                int32_t g, v = 0;
                if ((e = fm_map_char(f, (int32_t)c, &g)))
                    return e;
                if (g != -1) {
                    uint32_t idx = (uint32_t)g;
                    if (!fm_met.mapsize || idx < fm_met.mapsize) {
                        if (fm_met.mapsize)
                            idx = fm_met.map[idx];
                        v = idx < fm_met.nchars ? s16(arr + 2 * idx) : 0;
                        if (axis)
                            v = fm_scale_y(v);
                        else if (v)
                            v = fm_scale_x(v);
                    }
                }
                ros_st32(at, (uint32_t)v);
            }
        }
        s->r[2 + axis] = flags & none ? 0 : 4 * 256;
    }
    /* R4: the misc area, scaled */
    const uint8_t *misc = fm_met.misc, *kerns = fm_met.kerns;
    if (!misc || misc == kerns) {
        s->r[4] = 0;
    } else {
        if (r[4]) {
            uint32_t at = r[4];
            const uint8_t *p = misc;
            for (int i = 0; i < 7; i++, p += 2, at += 4)
                ros_st32(at, (uint32_t)(i & 1 ? fm_scale_y(s16(p)) : fm_scale_x(s16(p))));
            ros_st32(at, (uint32_t)(p[0] | p[1] << 8)), p += 2, at += 4;
            for (int i = 0; i < 4; i++, p += 2, at += 4)
                ros_st32(at, (uint32_t)fm_scale_y(s16(p)));
            for (; p < kerns; p++)
                ros_st8(at++, *p);
        }
        s->r[4] = (uint32_t)(kerns - misc) + 24;
    }
    /* R5, R6: the kerns, old style and as held */
    if ((e = fm_font_ptr(f->masterfont ? f->masterfont : fm_currentfont, &m)))
        return e;
    const uint8_t *blk;
    if ((e = fm_kerns(m, &blk)))
        return e;
    if (!blk) {
        s->r[5] = s->r[6] = 0;
    } else {
        uint32_t size;
        if ((e = old_kerns(f, m, blk, r[5], &size)))
            return e;
        s->r[5] = size;
        uint32_t n = m->kernsize;
        if (r[6])
            memcpy(ros_ptr(r[6]), blk, n);
        s->r[6] = n;
    }
    s->r[0] = flags;
    s->r[7] = 0;
    return NULL;
}

void fm_thunk_ReadFontMetrics(struct ros_cpu *s)
{
    uint32_t cur = fm_currentfont;
    os_error *e = read_font_metrics(s);
    fm_currentfont = cur;
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

/* ---- a font's data, on its deletion ------------------------------------------------------------- */

void fm_forget_metrics(struct fm_font *f)
{
    free(f->metrics), free(f->kerns);
    f->metrics = f->kerns = NULL;
    f->kernsize = 0;
}

void fm_forget_data(struct fm_font *f)
{
    fm_delete_chunks(&f->leaf1), fm_delete_chunks(&f->leaf4);
    fm_forget_transforms(f);
    free(f->metrics), free(f->kerns);
    free(f->leaf1.data), free(f->leaf4.data);
    f->metrics = f->kerns = f->leaf1.data = f->leaf4.data = NULL;
}
