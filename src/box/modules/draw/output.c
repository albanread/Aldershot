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
 * (Sources/Video/Render/DrawMod: s.DrOutput, s.DrQFill).
 */

/* output.c: the output stages (s/DrOutput and s/DrQFill). They write a
 * path, either in situ or to a buffer. They find a path's length and its
 * bounding box. They fill a path, either whole or subpath by subpath.
 *
 * A fill collects the path's edges in device coordinates (1/256 pixel) and
 * sorts them by their upper ends with the kernel's heapsort. It then scans
 * the graphics window row by row from the top. Edges are activated and
 * tracked with the original's Bresenham values. Each row's spans are
 * plotted through the VDU's exported HLine, in the foreground colour and
 * action. The general code decides boundary pixels by the inscribed
 * diamond. Styles &30-&33 and &0C-&0F take the original's fast code, which
 * only tracks crossings. Both follow the original register by register.
 *
 * ClippedFills: with F_BUFFER, a fill's spans are kept instead of plotted.
 * These are Draw_FillClipped's clipping path. With F_MASK, each span is
 * plotted where it meets a kept span on its row.
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/vdu.h"
#include "draw.h"

enum { LF_PARITY = 1, LF_DONE = 2, LF_ENDSHIGH = 4, LF_NOCROSS = 8, LF_NOTBDRY = 0x10 };

/* An edge is its flags and four words w[], then, once it is active, a second
 * area of five words. Before activation w[] holds lowerX, lowerY, upperX
 * and upperY. After activation w[0], w[1] and w[2] hold target, deltaX and
 * deltaY, and w[3] still holds upperY. */
struct area { int32_t bres, currX, leftX, rightX, crossX; };
struct edge {
    uint32_t flags;
    int32_t w[4];
    struct area a;
};

struct draw_edges {
    struct edge *e;
    uint32_t n, cap;
    uint32_t *ptr;                      /* the table of pointers, which holds edge numbers */
    uint32_t startactive, endwaiting, endpointers;
};

static int32_t asr(int32_t v, int n) { return v >> n; }

/* A 32-bit ADDS or SUBS. These give the wrapped result, and return the GE
 * condition, which is that the true result is >= 0. */
static int adds(int32_t *r, int32_t a, int32_t b)
{
    int64_t s = (int64_t)a + b;
    *r = (int32_t)(uint32_t)s;
    return s >= 0;
}

static int subs(int32_t *r, int32_t a, int32_t b)
{
    int64_t s = (int64_t)a - b;
    *r = (int32_t)(uint32_t)s;
    return s >= 0;
}

static int32_t dbl(int32_t v) { return (int32_t)((uint32_t)v << 1); }

/* ---- in situ, count, bounding box, path ------------------------------------------------ */

os_error *out_replace(struct draw *d, int i, struct draw_el *e)
{
    (void)i;
    e->x0 = e->x, e->y0 = e->y;
    uint32_t p = d->inputptr;
    switch (e->type) {
    case ET_MOVE: case ET_SPECIALMOVE: case ET_GAP: case ET_LINE:
        p -= 8;
        ros_st32(p, (uint32_t)e->x), ros_st32(p + 4, (uint32_t)e->y);
        /* fall through */
    case ET_CLOSEGAP: case ET_CLOSELINE:
        ros_st8(p - 4, e->type);
        break;
    case ET_BEZIER: {
        p -= 24;
        int32_t v[6] = { e->c1x, e->c1y, e->c2x, e->c2y, e->x, e->y };
        for (int k = 0; k < 6; k++)
            ros_st32(p + 4 * (uint32_t)k, (uint32_t)v[k]);
        ros_st8(p - 4, e->type);
        break;
    }
    default:
        break;
    }
    return NULL;
}

os_error *out_count(struct draw *d, int i, struct draw_el *e)
{
    (void)i;
    static const uint32_t len[9] = { 0, 0, 12, 12, 4, 4, 28, 12, 12 };
    e->x0 = e->x, e->y0 = e->y;
    if (e->type == ET_START)
        d->outcnt = 0;
    else if (e->type > ET_START)
        d->outcnt += len[e->type];
    else
        e->x0 = (int32_t)(d->outcnt + 8);
    return NULL;
}

void draw_initbox(struct draw *d)
{
    d->box[0] = d->box[1] = 0x7FFFFFFF;
    d->box[2] = d->box[3] = (int32_t)0x80000000;
}

void draw_updatebox(struct draw *d, int32_t x, int32_t y)
{
    if (d->box[0] > x) d->box[0] = x;
    if (d->box[1] > y) d->box[1] = y;
    if (d->box[2] < x) d->box[2] = x;
    if (d->box[3] < y) d->box[3] = y;
}

os_error *out_box(struct draw *d, int i, struct draw_el *e)
{
    (void)i;
    int32_t nx = e->x, ny = e->y;
    if (e->type == ET_END) {
        uint32_t at = d->outputtype;
        if (!(d->flags & F_R7IS32BIT))
            at &= 0x7FFFFFFF;
        for (int k = 0; k < 4; k++)
            ros_st32(at + 4 * (uint32_t)k, (uint32_t)d->box[k]);
    } else if (e->type == ET_START) {
        draw_initbox(d);
    } else {
        draw_updatebox(d, nx, ny);
        if (e->type == ET_BEZIER) {
            draw_updatebox(d, e->c1x, e->c1y);
            draw_updatebox(d, e->c2x, e->c2y);
        }
    }
    e->x0 = nx, e->y0 = ny;
    return NULL;
}

/* updatechangedbox: add this box (in pixels), clipped to the graphics
 * window, to the OS's ChangedBox. */
void draw_updatechangedbox(struct draw *d)
{
    int32_t b[4];
    memcpy(b, d->box, sizeof b);
    if (d->flags & F_FULLEXTERIOR) {
        b[0] = b[1] = (int32_t)0x80000000;
        b[2] = b[3] = 0x7FFFFFFF;
    }
    if (b[0] < d->lcol) b[0] = d->lcol;
    if (b[1] < d->brow) b[1] = d->brow;
    if (b[2] > d->rcol) b[2] = d->rcol;
    if (b[3] > d->trow) b[3] = d->trow;
    if (b[0] > b[2] || b[1] > b[3])
        return;
    uint32_t a = d->changedboxaddr + 4;
    int32_t c[4];
    for (int k = 0; k < 4; k++)
        c[k] = (int32_t)ros_ld32(a + 4 * (uint32_t)k);
    if (c[0] > b[0]) c[0] = b[0];
    if (c[1] > b[1]) c[1] = b[1];
    if (c[2] < b[2]) c[2] = b[2];
    if (c[3] < b[3]) c[3] = b[3];
    for (int k = 0; k < 4; k++)
        ros_st32(a + 4 * (uint32_t)k, (uint32_t)c[k]);
}

os_error *out_path(struct draw *d, int i, struct draw_el *e)
{
    (void)i;
    e->x0 = e->x, e->y0 = e->y;
    uint32_t need;
    switch (e->type) {
    case ET_END:
        ros_st32(d->outptr, 0);
        ros_st32(d->outptr + 4, d->outcnt);
        e->x0 = (int32_t)d->outptr;
        return NULL;
    case ET_START:
        d->outptr = d->outputtype;
        d->outcnt = ros_ld32(d->outptr + 4);
        return NULL;
    case ET_CLOSEGAP: case ET_CLOSELINE:
        need = 4;
        break;
    case ET_BEZIER:
        need = 28;
        break;
    default:
        need = 12;
        break;
    }
    if (d->outcnt < need) {
        d->outcnt -= need;
        return draw_err(DE_PATHFULL);
    }
    d->outcnt -= need;
    ros_st32(d->outptr, e->type);
    d->outptr += 4;
    if (e->type == ET_BEZIER) {
        int32_t v[4] = { e->c1x, e->c1y, e->c2x, e->c2y };
        for (int k = 0; k < 4; k++, d->outptr += 4)
            ros_st32(d->outptr, (uint32_t)v[k]);
    }
    if (need != 4) {
        ros_st32(d->outptr, (uint32_t)e->x), ros_st32(d->outptr + 4, (uint32_t)e->y);
        d->outptr += 8;
    }
    return NULL;
}

/* ---- the edge table ----------------------------------------------------------------------- */

void draw_freeworkspace(struct draw *d)
{
    if (d->edges) {
        free(d->edges->e);
        free(d->edges->ptr);
        free(d->edges);
        d->edges = NULL;
    }
}

static os_error *store_edge(struct draw *d, uint32_t flags, int32_t lx, int32_t ly, int32_t ux,
                            int32_t uy)
{
    struct draw_edges *t = d->edges;
    if (t->n == t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 64;
        struct edge *n = realloc(t->e, cap * sizeof *n);
        if (!n)
            return ros_error(0x101, "No room in RMA");
        t->e = n, t->cap = cap;
    }
    struct edge *g = &t->e[t->n++];
    memset(g, 0, sizeof *g);
    g->flags = flags;
    g->w[0] = lx, g->w[1] = ly, g->w[2] = ux, g->w[3] = uy;
    return NULL;
}

/* out_fill_init: make the table, with its dummy edge. The dummy edge is
 * never activated. */
static os_error *fill_init(struct draw *d)
{
    draw_freeworkspace(d);
    d->edges = calloc(1, sizeof *d->edges);
    if (!d->edges)
        return ros_error(0x101, "No room in RMA");
    return store_edge(d, 0, (int32_t)0x80000000, (int32_t)0x80000100, (int32_t)0x80000000,
                      (int32_t)0x80000100);
}

/* out_fill_addedge: add an edge to the table. */
static os_error *fill_addedge(struct draw *d, struct draw_el *e, uint32_t gap)
{
    int32_t nx = e->x, ny = e->y;
    int up = ny != e->y0 ? ny > e->y0 : e->x0 >= nx;
    uint32_t ox = (uint32_t)d->orgx << 8, oy = (uint32_t)d->orgy << 8;
    int32_t lx, ly, ux, uy;
    if (up) {
        lx = (int32_t)((uint32_t)e->x0 + ox), ly = (int32_t)((uint32_t)e->y0 + oy);
        ux = (int32_t)((uint32_t)nx + ox), uy = (int32_t)((uint32_t)ny + oy);
    } else {
        lx = (int32_t)((uint32_t)nx + ox), ly = (int32_t)((uint32_t)ny + oy);
        ux = (int32_t)((uint32_t)e->x0 + ox), uy = (int32_t)((uint32_t)e->y0 + oy);
    }
    lx >>= d->xeig, ly >>= d->yeig, ux >>= d->xeig, uy >>= d->yeig;
    int32_t w = up ? (int32_t)(0u - (uint32_t)d->currentwinding) : d->currentwinding;
    uint32_t flags = gap | ((uint32_t)w << 30) >> 2;
    if (ros_ld32(d->changedboxaddr) & 1) {
        draw_updatebox(d, ux >> 8, uy >> 8);
        draw_updatebox(d, lx >> 8, ly >> 8);
    }
    e->x0 = nx, e->y0 = ny;
    if (d->rcol < lx >> 8 && d->rcol < ux >> 8)
        return NULL;
    if (d->brow > ly >> 8 && d->brow > uy >> 8)
        return NULL;
    if (d->trow < ly >> 8 && d->trow < uy >> 8)
        return NULL;
    return store_edge(d, flags, lx, ly, ux, uy);
}

/* ---- plotting ------------------------------------------------------------------------------ */

struct scan {
    struct draw *d;
    int32_t r0;                         /* the left end of the span being plotted */
    int32_t y;                          /* R1 */
    uint32_t fs;                        /* R5: the modified fill style and state */
};

static void hline(struct scan *s, int32_t a, int32_t b)
{
    if (s->d->spanhook)
        s->d->spanhook(s->d, a, s->y, b);
    else
        ros_vdu_hline(a, s->y, b, 1);
}

/* out_fill_clippedchange: a change of plotting state at x, when buffering
 * spans or masking them. */
static void clippedchange(struct scan *s, int32_t x)
{
    struct draw *d = s->d;
    if (s->fs & F_BUFFER) {
        if (d->clipused + 12 > d->clipsize) {
            int32_t *n = realloc(d->clip, d->clipsize + 256);
            if (!n)
                return;
            d->clip = n, d->clipsize += 256;
        }
        int32_t *c = (int32_t *)((uint8_t *)d->clip + d->clipused);
        c[0] = s->r0, c[1] = s->y, c[2] = x;
        d->clipused += 12, d->clipcount++;
        return;
    }
    for (uint32_t k = 0; k + 12 <= d->clipused; k += 12) {
        const int32_t *c = (const int32_t *)((const uint8_t *)d->clip + k);
        if (c[1] != s->y || c[0] > x || c[2] < s->r0)
            continue;
        int32_t l = c[0] > s->r0 ? c[0] : s->r0, r = c[2] < x ? c[2] : x;
        hline(s, l, r - 1);
    }
}

/* out_fill_plottingchange: enter or leave the plotting state at x. */
static void plottingchange(struct scan *s, int32_t x)
{
    int was = (s->fs & F_FULLEXTERIOR) != 0;
    s->fs ^= F_FULLEXTERIOR;
    if (!was) {
        s->r0 = x;
        return;
    }
    if (s->r0 == x)
        return;
    if (s->fs & (F_BUFFER | F_MASK)) {
        clippedchange(s, x);
        return;
    }
    hline(s, s->r0, x - 1);
}

/* Whether winding number w is exterior under the style's rule. */
static int exterior(uint32_t fs, int32_t w)
{
    int32_t v = w;
    switch (fs & 3) {
    case 0: break;
    case 1: v = asr(v, 31); break;
    case 2: v &= 1; break;
    default: v = asr((int32_t)(0u - (uint32_t)v), 31) & 1; break;
    }
    return v == 0;
}

static int32_t wind(uint32_t flags) { return (int32_t)(flags << 2) >> 30; }
static int32_t dirn(uint32_t flags) { return (int32_t)flags >> 30; }

/* ---- the general fill ---------------------------------------------------------------------- */

/* out_fill_fasthoriz: go a long way along a flat edge by division. r6 must
 * be >= 0 and r4 must not be 0. */
static void fasthoriz(uint32_t r0, int32_t r2, int32_t r4, int32_t *r6, int32_t *r7)
{
    uint32_t two = (uint32_t)r4 << 1;
    uint32_t q = (uint32_t)*r6 / two;
    *r6 = (int32_t)((uint32_t)*r6 % two);
    int32_t v;
    if ((int32_t)r0 < 0) {
        int64_t t = (int64_t)*r7 - (int32_t)q;
        v = t != (int32_t)t ? r2 : (int32_t)t;
        if (v < r2)
            v = r2;
    } else {
        int64_t t = (int64_t)*r7 + (int32_t)q;
        v = t != (int32_t)t ? r2 : (int32_t)t;
        if (v > r2)
            v = r2;
    }
    *r7 = v;
}

/* out_fill_doscan: one scan line of an edge. r0 is the edge's flags. They
 * are changed here, and stored in the edge where the original stores them.
 * r11 is what the original's R11 holds on entry. A horizontal edge leaves
 * it as its crossing. */
static void doscan(struct scan *s, struct edge *g, uint32_t r0, int32_t r6, int32_t r7, int32_t r11)
{
    int32_t r1 = s->y, r2 = g->w[0], r3 = g->w[1], r4 = g->w[2];
    int32_t dir = dirn(r0), r8 = r7, r10;
    int ydone = 0, z = 0;
    if (r4 >= r3) {                                    /* major Y */
        r10 = r7 + dir;
        int32_t t;
        r11 = adds(&t, r6, r4) ? r10 : r7;
        if (adds(&r6, r6, dbl(r3))) {
            r7 = r10;
            r6 = (int32_t)((uint32_t)r6 - (uint32_t)dbl(r4));
        }
        if (r1 == r2)
            ydone = 1, z = 1;
    } else if (r4 == 0) {
        r7 = r2;
        goto xdone2;
    } else {
        if (!subs(&r6, r6, r3))
            goto xskip1;
        if (r4 <= asr(r6, 4))
            fasthoriz(r0, r2, r4, &r6, &r7);
        for (;;) {
            if (r7 == r2) {
                r11 = r7 + dir;
                goto xdone2;
            }
            r7 += dir;
            if (!subs(&r6, r6, dbl(r4)))
                break;
        }
    xskip1:
        r11 = r7;
        if (adds(&r6, r6, r3)) {
            if (r4 <= asr(r6, 4))
                fasthoriz(r0, r2, r4, &r6, &r7);
            for (;;) {
                if (r7 == r2)
                    goto xdone2;
                r7 += dir;
                if (!subs(&r6, r6, dbl(r4)))
                    break;
            }
        }
        r6 = (int32_t)((uint32_t)r6 + (uint32_t)dbl(r3));
        r10 = r7;
        goto tidy;
    xdone2:
        z = ((r1 ^ (int32_t)r0) & 1) == 0;
        r10 = r7 + dir;
        ydone = 1;
    }
    if (ydone) {
        if (!z || (r0 & LF_ENDSHIGH))
            r0 |= LF_NOCROSS;
        r0 |= LF_DONE;
        g->flags = r0;
    }
tidy:
    if ((int32_t)r0 < 0) {
        int32_t t = r8;
        r8 = r10 + 1, r10 = t + 1;
        r11++;
    }
    if (r0 & LF_NOTBDRY)
        r8 = r10 = r11;
    if (r0 & LF_NOCROSS)
        r11 = 0x7FFFFFFF;
    g->a.bres = r6, g->a.currX = r7, g->a.leftX = r8, g->a.rightX = r10, g->a.crossX = r11;
}

/* Find the pixel that holds an endpoint, and the endpoint's position within
 * that pixel. The activation of both algorithms does this. */
static void endpoint(uint32_t r0, int32_t v, int32_t w, int32_t *px, int32_t *py, int32_t *sx,
                     int32_t *sy)
{
    int32_t x = v & 255, y = w & 255;
    *px = v >> 8, *py = w >> 8;
    if ((int32_t)r0 < 0)
        x = 255 - x;
    y -= 128;
    if (!(x >= y && x + y >= 0))
        *px -= dirn(r0);
    else
        x -= 256;
    if (x + y >= 0)
        (*py)++, y -= 256;
    *sx = x, *sy = y;
}

/* out_fill_fastclip: skip r8 rows (r8 > 0) in one step. */
static void fastclip(uint32_t r0, int32_t r3, int32_t r4, int32_t r8, int32_t *r6, int32_t *r7)
{
    int64_t acc = (int64_t)*r6 + (int64_t)dbl(r8) * r3;
    uint64_t u = (uint64_t)acc;
    *r6 = (int32_t)(uint32_t)u;
    r8 = 0;
    if (acc >= 0) {
        r8 = (int32_t)draw_dsdivs((uint32_t)u, (uint32_t)(u >> 32), (uint32_t)r4) & ~1;
        if (r4 >= r3)
            r8 += 2;
        *r6 = (int32_t)((uint32_t)*r6 - (uint32_t)((int64_t)r8 * r4));
    }
    if ((int32_t)r0 >= 0)
        *r7 += r8 >> 1;
    else
        *r7 -= r8 >> 1;
}

/* Activate edge g on row r1 (the general code). */
static void activate(struct scan *s, struct edge *g)
{
    uint32_t r0 = g->flags;
    int32_t r1 = s->y, r5 = g->w[0], r6 = g->w[1], r7 = g->w[2], r8 = g->w[3];
    int32_t r3, r4 = (int32_t)((uint32_t)r8 - (uint32_t)r6);
    int ge = r7 >= r5;
    r3 = (int32_t)((uint32_t)r7 - (uint32_t)r5);
    if (!ge)
        r3 = (int32_t)(0u - (uint32_t)r3);
    r0 |= 0x40000000;
    if (ge)
        r0 |= 0x80000000;
    int32_t lpx, lpy, lsx, lsy;
    endpoint(r0, r5, r6, &lpx, &lpy, &lsx, &lsy);
    if (lsy >= 0)
        r0 |= LF_ENDSHIGH;
    if (lpy & 1)
        r0 |= LF_PARITY;
    int32_t r2 = r4 >= r3 ? lpy : lpx;
    g->flags = r0, g->w[0] = r2, g->w[1] = r3, g->w[2] = r4;
    /* the upper end: the current pixel and the Bresenham value */
    int32_t r10, r11;
    endpoint(r0, r7, r8, &r7, &r8, &r10, &r11);
    uint32_t c = r0 >> 31;
    if ((uint32_t)r3 + (uint32_t)r4 < 0x800000u) {
        uint32_t v = (uint32_t)((int32_t)r0 >> 31);
        v += (uint32_t)(r10 + 128 + (int32_t)c) * (uint32_t)r4;
        v += (uint32_t)r11 * (uint32_t)r3;
        r6 = (int32_t)v >> 7;
    } else {
        int64_t v = (int64_t)((int32_t)r0 >> 31);
        v += (int64_t)(r10 + 128 + (int32_t)c) * r4;
        v += (int64_t)r11 * r3;
        r6 = (int32_t)(uint32_t)(v >> 7);
    }
    if (r4 >= r3)
        r6 = (int32_t)((uint32_t)r6 - (uint32_t)r4);
    else
        r6 = (int32_t)((uint32_t)r6 + (uint32_t)r3);
    int32_t dir = dirn(r0);
    int64_t rows = (int64_t)r8 - r1;
    if (rows > 0) {                             /* FastYClipping */
        if (rows - 1 > 0)
            fastclip(r0, r3, r4, (int32_t)(rows - 1), &r6, &r7);
        if (r4 < r3) {
            if (r4 != 0 && r4 <= asr(r6, 4))
                fasthoriz(r0, r2, r4, &r6, &r7);
            for (;;) {
                if (r7 == r2)
                    goto dies;
                r7 += dir;
                if (!subs(&r6, r6, dbl(r4)))
                    break;
            }
            r6 = (int32_t)((uint32_t)r6 + (uint32_t)dbl(r3));
        } else {
            if (r1 < r2)
                goto dies;
            if (adds(&r6, r6, dbl(r3))) {
                r7 += dir;
                r6 = (int32_t)((uint32_t)r6 - (uint32_t)dbl(r4));
            }
        }
    } else {                                    /* out_fill_noclipping */
        if (r11 < 0)
            r0 |= LF_NOCROSS;
        if (!(r10 < r11)) {
            if (r4 >= r3) {
                if (r1 == r2)
                    goto dies;
                if (adds(&r6, r6, dbl(r3))) {
                    r7 += dir;
                    r6 = (int32_t)((uint32_t)r6 - (uint32_t)dbl(r4));
                }
                goto emptydone;
            }
            if (r7 == r2)
                goto dies;
            r7 += dir;
            if (!subs(&r6, r6, dbl(r4)))
                goto empty;
        }
    }
    doscan(s, g, r0, r6, r7, r11);
    return;
dies:
    r0 |= LF_DONE | LF_NOCROSS;
    g->flags = r0;
empty:
    r6 = (int32_t)((uint32_t)r6 + (uint32_t)dbl(r3));
emptydone:
    g->a.bres = r6, g->a.currX = r7, g->a.leftX = r7, g->a.rightX = r7;
    g->a.crossX = 0x7FFFFFFF;
}

/* Bubble sort of the active edges, upwards and stable, by leftX or by
 * crossX. */
static void sort_active(struct draw_edges *t, int q)
{
    uint32_t lo = t->startactive, hi = t->endpointers;
    if (hi <= lo)
        return;
    uint32_t r9 = hi;
    for (;;) {
        uint32_t r8 = r9 - 1;
        r9 = lo;
        uint32_t r7 = lo;
        if (r7 >= r8)
            return;
        uint32_t r3 = t->ptr[r7];
        int32_t r4 = q ? t->e[r3].a.crossX : t->e[r3].a.leftX;
        do {
            uint32_t r5 = t->ptr[++r7];
            int32_t r6 = q ? t->e[r5].a.crossX : t->e[r5].a.leftX;
            if (r4 <= r6) {
                t->ptr[r7 - 1] = r3;
                r3 = r5, r4 = r6;
            } else {
                t->ptr[r7 - 1] = r5;
                r9 = r7;
            }
        } while (r7 < r8);
        t->ptr[r7] = r3;
    }
}

static int upper_less(uint32_t x, uint32_t y, void *ctx)
{
    const struct draw_edges *t = ctx;
    return t->e[x].w[3] < t->e[y].w[3];
}

/* The general boundary scan's plotting of a row. */
static void plot_row(struct scan *s)
{
    struct draw *d = s->d;
    struct draw_edges *t = d->edges;
    uint32_t r10 = t->startactive, r11 = t->endpointers;
    int32_t r6 = 0;
    s->r0 = (int32_t)0x80000000;
    while (r10 < r11) {
        uint32_t r9 = r10;
        int32_t r7 = 0x7FFFFFFF, r8 = (int32_t)0x80000000, r4 = 0;
        do {
            struct edge *g = &t->e[t->ptr[r10++]];
            int32_t r2 = g->a.leftX, r3 = g->a.rightX;
            if (r2 > r8 && r2 > r7) {
                r10--;
                break;
            }
            if (r7 > r2)
                r7 = r2;
            if (r8 < r3)
                r8 = r3;
            if (g->a.crossX != 0x7FFFFFFF)
                r4 += wind(g->flags);
        } while (r10 < r11);
        if (r7 > d->rcol)
            break;
        if (s->fs & (s->fs & F_INSIDE ? F_FULLINTERIOR : F_EXTERIORBDRY))
            plottingchange(s, r7);
        if (!(s->fs & F_INTERIORBDRY)) {
            r6 += r4;
            if (exterior(s->fs, r6))
                s->fs &= ~(uint32_t)F_INSIDE;
            else
                s->fs |= F_INSIDE;
        } else {
            for (;;) {                          /* the hard case, in which r4 carries on */
                int32_t r2 = 0x7FFFFFFE;
                for (uint32_t r3 = r9; r3 < r10; r3++) {
                    struct edge *g = &t->e[t->ptr[r3]];
                    int32_t x = g->a.crossX;
                    if (x < r7)
                        continue;
                    int ge = r2 >= x;
                    if (r2 > x)
                        r2 = x, r4 = 0;
                    if (ge)
                        r4 += wind(g->flags);
                }
                if (r2 == 0x7FFFFFFE)
                    break;
                r7 = r2 + 1;
                r6 += r4;
                int ext = exterior(s->fs, r6), in = (s->fs & F_INSIDE) != 0;
                if (ext == !in)
                    continue;
                s->fs ^= F_INSIDE;
                plottingchange(s, r2);
            }
        }
        if (s->fs & (s->fs & F_INSIDE ? F_FULLINTERIOR : F_EXTERIORBDRY))
            plottingchange(s, r8);
    }
    plottingchange(s, 0x7FFFFFFF);
}

/* The fast fill's activation of edge g on row r1 (DrQFill). */
static void qactivate(struct scan *s, struct edge *g)
{
    uint32_t r0 = g->flags;
    int32_t r1 = s->y, r5 = g->w[0], r6 = g->w[1] - 128, r7 = g->w[2], r8 = g->w[3] - 128;
    int ge = r7 >= r5;
    int32_t r3 = (int32_t)((uint32_t)r7 - (uint32_t)r5);
    if (!ge)
        r3 = (int32_t)(0u - (uint32_t)r3);
    int32_t r4 = (int32_t)((uint32_t)r8 - (uint32_t)r6);
    r0 |= 0x40000000;
    if (ge)
        r0 |= 0x80000000;
    int32_t r2 = (r6 >> 8) + 1;
    g->flags = r0, g->w[0] = r2, g->w[1] = r3, g->w[2] = r4;
    int32_t r14 = r7 & 255;
    r7 >>= 8;
    uint32_t c = r0 >> 31;
    int32_t sx = r14 - 128;
    if (c)
        sx = -sx;
    if ((uint32_t)r3 + (uint32_t)r4 < 0x800000u) {
        uint32_t v = (uint32_t)((int32_t)r0 >> 31);
        v += (uint32_t)sx * (uint32_t)r4;
        v += (uint32_t)(r8 & 255) * (uint32_t)r3;
        r6 = (int32_t)v >> 8;
    } else {
        int64_t v = (int64_t)((int32_t)r0 >> 31);
        v += (int64_t)sx * r4;
        v += (int64_t)(r8 & 255) * r3;
        r6 = (int32_t)(uint32_t)(v >> 8);
    }
    r8 >>= 8;
    int32_t dir = dirn(r0);
    int go = 0;
    if (r1 >= r2) {
        int32_t n = r8 - r1;
        if (n > 0) {                            /* out_qfill_fastclip */
            int64_t acc = (int64_t)r6 + (int64_t)n * r3;
            uint64_t u = (uint64_t)acc;
            int32_t k;
            if (acc < 0) {
                k = 0;
                r6 = (int32_t)(uint32_t)acc;
            } else {
                k = (int32_t)draw_dsdivs((uint32_t)u, (uint32_t)(u >> 32), (uint32_t)r4);
                r6 = (int32_t)((uint32_t)acc - (uint32_t)((int64_t)k * r4));
            }
            r7 += (int32_t)r0 >= 0 ? k : -k;
        }
        if (n >= 0)
            go = r6 >= 0;
    }
    while (go) {
        r7 += dir;
        go = subs(&r6, r6, r4);
    }
    r6 = (int32_t)((uint32_t)r6 + (uint32_t)r3);
    int32_t cross = r7 - ((int32_t)r0 >> 31);
    if (r1 >= r2) {
        if (r4 <= asr(r6, 3)) {
            uint32_t q = (uint32_t)r6 / (uint32_t)r4;
            r6 = (int32_t)((uint32_t)r6 % (uint32_t)r4);
            r7 += (int32_t)r0 >= 0 ? (int32_t)q : -(int32_t)q;
        }
        go = r6 >= 0;
        while (go) {
            r7 += dir;
            go = subs(&r6, r6, r4);
        }
    }
    g->a.leftX = r6, g->a.rightX = r7, g->a.crossX = cross;   /* these are qbres, qcurrX and qcrossX */
}

/* The fast fill's advance of an active edge. */
static void qadvance(struct scan *s, struct edge *g)
{
    uint32_t r0 = g->flags;
    int32_t r1 = s->y, r2 = g->w[0], r3 = g->w[1], r4 = g->w[2];
    int32_t r6 = g->a.leftX, r7 = g->a.rightX;
    int32_t r8 = r7 - ((int32_t)r0 >> 31);
    r6 = (int32_t)((uint32_t)r6 + (uint32_t)r3);
    if (r1 >= r2) {
        if (r4 <= asr(r6, 3)) {
            uint32_t q = (uint32_t)r6 / (uint32_t)r4;
            r6 = (int32_t)((uint32_t)r6 % (uint32_t)r4);
            r7 += (int32_t)r0 >= 0 ? (int32_t)q : -(int32_t)q;
        }
        int go = r6 >= 0;
        while (go) {
            r7 += dirn(r0);
            go = subs(&r6, r6, r4);
        }
    }
    g->a.leftX = r6, g->a.rightX = r7, g->a.crossX = r8;
}

/* out_fill_dofill and out_qfill: scan the edges and plot the fill. */
static void dofill(struct draw *d)
{
    struct draw_edges *t = d->edges;
    uint32_t n = t->n;
    t->ptr = malloc(n * sizeof *t->ptr);
    if (!t->ptr)
        return;
    for (uint32_t k = 0; k < n; k++)
        t->ptr[k] = k;
    t->startactive = t->endwaiting = t->endpointers = n;
    draw_heapsort(n, t->ptr, upper_less, t);
    uint32_t fs = d->flags ^ ((d->flags & (F_FULLEXTERIOR | F_EXTERIORBDRY | F_INTERIORBDRY)) << 1);
    d->fillstyle = fs;
    struct scan s = { d, 0, d->trow, fs };
    int q = (fs & (F_EXTERIORBDRY | F_INTERIORBDRY | F_FULLINTERIOR)) == F_INTERIORBDRY;
    do {
        if (t->endpointers <= t->startactive && !(fs & F_FULLEXTERIOR)) {
            /* possibly skip to the row of the next edge's upper end */
            int32_t uy = t->e[t->ptr[t->endwaiting - 1]].w[3];
            int32_t row = q ? (uy - 128) >> 8 : uy >> 8;
            if (s.y > row) {
                s.y = row;
                if (d->brow > s.y)
                    break;
            }
        }
        for (uint32_t r11 = t->endpointers; r11 > t->startactive;) {
            struct edge *g = &t->e[t->ptr[--r11]];
            if (q)
                qadvance(&s, g);
            else                                /* (the original's R11 holds a pointer here, which is never a crossing) */
                doscan(&s, g, g->flags, g->a.bres, g->a.currX, 0);
        }
        uint32_t r10 = t->endwaiting, r11 = t->startactive;
        for (;;) {
            uint32_t k = t->ptr[--r10];
            struct edge *g = &t->e[k];
            int32_t uy = g->w[3];
            if (s.y > (q ? (uy - 128) >> 8 : uy >> 8))
                break;
            t->ptr[--r11] = k;
            if (q)
                qactivate(&s, g);
            else
                activate(&s, g);
        }
        t->endwaiting = r10 + 1, t->startactive = r11;
        if (q) {
            /* remove the finished edges, then sort by crossing */
            uint32_t r9 = t->endpointers;
            for (uint32_t k = t->endpointers; k > t->startactive;) {
                uint32_t p = t->ptr[--k];
                if (s.y >= t->e[p].w[0])
                    t->ptr[--r9] = p;
            }
            t->startactive = r9;
            sort_active(t, 1);
            s.fs = fs;
            s.r0 = (int32_t)0x80000000;
            int32_t w = 0;
            for (uint32_t k = t->startactive; k < t->endpointers; k++) {
                struct edge *g = &t->e[t->ptr[k]];
                int32_t x = g->a.crossX;
                if (x > d->rcol)
                    break;
                w += wind(g->flags);
                uint32_t ch = s.fs & F_INSIDE;
                if (!exterior(s.fs, w))
                    ch ^= F_INSIDE;
                s.fs ^= ch;
                if (ch)
                    plottingchange(&s, x);
            }
            plottingchange(&s, 0x7FFFFFFF);
        } else {
            sort_active(t, 0);
            s.fs = fs;
            plot_row(&s);
            uint32_t r9 = t->endpointers;
            for (uint32_t k = t->endpointers; k > t->startactive;) {
                uint32_t p = t->ptr[--k];
                if (!(t->e[p].flags & LF_DONE))
                    t->ptr[--r9] = p;
            }
            t->startactive = r9;
        }
    } while (--s.y >= d->brow);
}

/* ---- the fill outputs ------------------------------------------------------------------------ */

void draw_scan(struct draw *d)
{
    dofill(d);
}

uint32_t draw_edge_count(const struct draw *d)
{
    return d->edges ? d->edges->n : 0;
}

int32_t draw_edge(const struct draw *d, uint32_t k, int32_t w[4])
{
    const struct edge *g = &d->edges->e[k];
    for (int j = 0; j < 4; j++)
        w[j] = g->w[j];
    return wind(g->flags);
}

/* The fill, done by Draw's scan or by GDraw's hook. */
static os_error *fill_now(struct draw *d)
{
    if (d->fillhook)
        return d->fillhook(d);
    dofill(d);
    return NULL;
}

os_error *out_fill(struct draw *d, int i, struct draw_el *e)
{
    (void)i;
    os_error *err;
    switch (e->type) {
    case ET_END:
        err = fill_now(d);
        draw_freeworkspace(d);
        return err;
    case ET_START:
        return fill_init(d);
    case ET_MOVE: case ET_SPECIALMOVE:
        d->currentwinding = e->type == ET_SPECIALMOVE ? 0 : d->userspacewinding;
        e->x0 = e->x, e->y0 = e->y;
        return NULL;
    case ET_CLOSEGAP: case ET_GAP:
        return fill_addedge(d, e, LF_NOTBDRY);
    case ET_BEZIER:
        return draw_err(DE_NOTFLAT);
    default:
        return fill_addedge(d, e, 0);
    }
}

os_error *out_subpaths(struct draw *d, int i, struct draw_el *e)
{
    os_error *err;
    switch (e->type) {
    case ET_END:
        err = NULL;
        if (d->gotasubpath) {
            err = fill_now(d);
            draw_freeworkspace(d);
        }
        return err;
    case ET_START:
        d->gotasubpath = 0;
        return fill_init(d);
    case ET_MOVE: case ET_SPECIALMOVE:
        if (d->gotasubpath && (err = fill_now(d)))
            return err;
        d->gotasubpath = 0;
        if ((err = fill_init(d)))
            return err;
        d->gotasubpath = 1;
        return out_fill(d, i, e);
    case ET_BEZIER:
        return draw_err(DE_NOTFLAT);
    default:
        d->gotasubpath = 1;
        return out_fill(d, i, e);
    }
}

/* clipped_sort: put the clipping path's spans last first. (The original's
 * sort compares the blocks' addresses, not their contents.) */
void draw_clip_sort(struct draw *d)
{
    uint32_t n = d->clipcount;
    for (uint32_t a = 0, b = n ? n - 1 : 0; a < b; a++, b--) {
        int32_t *p = d->clip + 3 * a, *q = d->clip + 3 * b, t[3];
        memcpy(t, p, sizeof t), memcpy(p, q, sizeof t), memcpy(q, t, sizeof t);
    }
}
