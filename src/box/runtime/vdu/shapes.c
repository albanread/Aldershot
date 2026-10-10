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
 * This file is a transliteration into C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.vdu.vdugrafb, s.vdu.vdugrafc, s.vdu.vduplot).
 */
/* shapes.c -- circles, arcs, segments, sectors and ellipses (PLOT 144-183,
 * 192-207).
 *
 * This is transliterated from the kernel's Kernel/s/vdu/vdugrafb,
 * vdugrafc and vduplot (GenCircleParm, AdvCircleParm and SquareRoot). It is
 * not re-derived. The pixels depend on the kernel's integer square roots
 * and its circle stepping. For arcs, segments and sectors they also depend
 * on a state machine for each quadrant, driven by a table of control bytes.
 * The state machine's quirks are copied as they are.
 *
 *   - Circles. The centre is the previous point, and the radius runs to
 *     this point. The radius is floor(sqrt(r^2 + floor(sqrt(r^2)))).
 *     Non-square pixels double one distance (AspectRatio).
 *   - Arcs, segments and sectors. The centre is the point before the
 *     previous one. The radius and start come from the previous point. The
 *     arc runs anticlockwise to the direction of this point.
 *   - Ellipses. The centre is the point before the previous one. The
 *     previous point's x gives the half-width. This point gives the height
 *     and the shear.
 *
 * Where the kernel could loop for ever (a ray that never reaches its row),
 * the loops here are bounded.
 */
#include <string.h>

#include "vduws.h"
#include "plot.h"

#define GUARD (1u << 24)

/* ---- square roots ------------------------------------------------------------ */

static uint32_t sqrt_alt(uint32_t n, int iterations)
{
    uint32_t result = 0, sqdiff = 0;
    while (iterations--) {
        sqdiff = sqdiff << 2 | n >> 30;
        n <<= 2;
        uint32_t trial = (result << 2) + 1;
        if (sqdiff >= trial) {
            sqdiff -= trial;
            result = result << 1 | 1;
        } else {
            result <<= 1;
        }
    }
    return result;
}

static uint32_t sqrt16(uint32_t n)
{
    return sqrt_alt(n, 16);
}

/* ---- the circle ---------------------------------------------------------------- */

struct circle {
    int32_t x, y, sum, up, down, cx, cy, aspect;
};

static struct circle cb;
static uint32_t rad_square;             /* CircleRadSquare */

static void gen_circle(struct circle *b, int32_t cx, int32_t cy, int32_t px, int32_t py)
{
    int32_t a = (int32_t)vdu.aspect;
    uint32_t dx = (uint32_t)(px - cx), dy = (uint32_t)(py - cy);
    if (a & 1)
        dx <<= 1;
    if (a & 2)
        dy <<= 1;
    uint32_t raw = dx * dx + dy * dy;
    uint32_t radsqr = raw + sqrt16(raw);
    rad_square = radsqr;
    uint32_t rad = sqrt16(radsqr);
    if (VDU_CLIPPING)
        vdu_cbox_circle(rad, cx, cy);
    b->sum = (int32_t)(radsqr - rad * rad);
    b->cx = cx, b->cy = cy;
    b->x = (int32_t)rad;
    b->down = (int32_t)(2 * rad - 1);
    b->y = 0;
    b->up = 1;
    b->aspect = a;
    if (a == 1) {
        b->x = (int32_t)rad >> 1;
        b->sum += b->down;
        b->down -= 2;
    } else if (a > 1) {
        b->sum -= b->up;
        b->up += 2;
    }
}

/* Advances the circle. It returns 1 if y changed. */
static int adv_circle(struct circle *b)
{
    if (!(b->sum >= b->up)) {
        b->x -= 1;
        b->sum += b->down;
        b->down -= 2;
        if (b->aspect & 1) {
            b->sum += b->down;
            b->down -= 2;
        }
        if (!(b->sum >= b->up))
            return 0;
    }
    b->y += 1;
    b->sum -= b->up;
    b->up += 2;
    if (b->aspect & 2) {
        b->sum -= b->up;
        b->up += 2;
    }
    return 1;
}

void plot_circle_outline(void)
{
    gen_circle(&cb, vdu.gcsix, vdu.gcsiy, vdu.newptx, vdu.newpty);
    for (;;) {
        int32_t x = cb.x, y = cb.y, cx = cb.cx, cy = cb.cy;
        plot_point(cx + x, cy + y);
        if (x != 0)
            plot_point(cx - x, cy + y);
        if (y != 0) {
            plot_point(cx + x, cy - y);
            if (x != 0)
                plot_point(cx - x, cy - y);
        }
        if (cb.x == 0)
            return;
        adv_circle(&cb);
    }
}

void plot_circle_fill(void)
{
    gen_circle(&cb, vdu.gcsix, vdu.gcsiy, vdu.newptx, vdu.newpty);
    for (;;) {
        int32_t l = cb.cx - cb.x, r = cb.cx + cb.x;
        plot_new_hline(l, cb.cy + cb.y, r);
        if (cb.y != 0)
            plot_new_hline(l, cb.cy - cb.y, r);
        do {
            if (cb.x == 0)
                return;
        } while (!adv_circle(&cb));
    }
}

/* ---- arcs: the quadrant state machine ---------------------------------------------- */

/* CLine0 is the start ray, CLine1 is the end ray, and CLine2 and CLine3
 * are the segment chord. In CLine0 and CLine1, ex doubles as "Near" once
 * stepping has begun. */
static struct plot_line cl[4];
static uint8_t control[4], draw[4], change[4];
static struct { int32_t x, y; } ap[4];  /* ArcPoint0-3 */

static const uint32_t gen_arc_tb[24] = {
    0x0000003A, 0x01010307, 0x03000007, 0x010A0007,
    0x00000E0A, 0x00001E00, 0x03000E01, 0x010A0E01,
    0x0E01010A, 0x0E010300, 0x1E000000, 0x0E0A0000,
    0x0007010A, 0x00070300, 0x03070101, 0x003A0000,
    0x01010157, 0x00001E00, 0x1E000000, 0x01570101,
    0x0000003A, 0x01017301, 0x73010101, 0x003A0000,
};

static uint32_t quadrant(const struct plot_line *l)
{
    return (l->sx < 0 ? 4u : 0u) | (l->sy < 0 ? 8u : 0u);
}

static void gen_arc(void)
{
    gen_circle(&cb, vdu.oldx, vdu.oldy, vdu.gcsix, vdu.gcsiy);
    plot_gen_line(&cl[0], vdu.oldx, vdu.oldy, vdu.gcsix, vdu.gcsiy);
    uint32_t s = quadrant(&cl[0]);
    int32_t ex = vdu.newptx, ey = vdu.newpty;
    if (vdu.oldx == ex && vdu.oldy == ey)
        ex += 1;                        /* "compatibility with Master" */
    plot_gen_line(&cl[1], vdu.oldx, vdu.oldy, ex, ey);
    uint32_t e = quadrant(&cl[1]), idx;
    if (s != e) {
        idx = s | e << 2;
    } else {
        int32_t p = (int32_t)((uint32_t)cl[0].dx * (uint32_t)cl[1].dy);
        int32_t q = (int32_t)((uint32_t)cl[0].dy * (uint32_t)cl[1].dx);
        idx = p < q ? (e | 0x40) : p > q ? (e | 0x50) : (e | e << 2);
    }
    uint32_t w = gen_arc_tb[idx / 4];
    for (int i = 0; i < 4; i++)
        control[i] = (uint8_t)(w >> (8 * i));
}

static void reflect(void)
{
    ap[0].x = cb.cx + cb.x, ap[0].y = cb.cy + cb.y;
    ap[1].x = cb.cx - cb.x, ap[1].y = cb.cy + cb.y;
    ap[2].x = cb.cx - cb.x, ap[2].y = cb.cy - cb.y;
    ap[3].x = cb.cx + cb.x, ap[3].y = cb.cy - cb.y;
}

/* ArcLineStep. It returns 0 within the circle, 1 on it, or 2 beyond. */
static int arc_line_step(struct plot_line *l, int32_t cx, int32_t cy)
{
    for (uint32_t g = 0; l->y != cy && g < GUARD; g++)
        plot_adv_line(l);
    int32_t nearx = l->x, d = l->x - cx;
    int r;
    if (d == 0) {
        r = 1;
    } else if ((d ^ l->sx) >= 0) {
        l->x = cx;
        r = (d ^ l->sx) == 0 ? 1 : 2;   /* one pixel beyond counts as on */
    } else {
        for (uint32_t g = 0;; g++) {
            if (l->bres >= 0) {
                l->bres -= l->dy;
                l->x += l->sx;
            }
            if (l->x == cx) {
                r = 1;
                break;
            }
            if (l->bres < 0 || g >= GUARD) {
                r = 0;
                break;
            }
        }
    }
    l->ex = nearx;
    return r;
}

static void update_quadrant(int q)
{
    uint8_t c = control[q];
    if (!(c & 2))
        return;
    int r = arc_line_step(&cl[c & 4 ? 1 : 0], ap[q].x, ap[q].y);
    uint8_t n = c >> 3;
    if (r >= 1)
        change[q] = 1, control[q] = n;
    if (r == 2 || (r == 1 && (n & 1)))
        draw[q] = n;
    if (!(n & 2))
        return;                         /* tested on n even with no hit, as the kernel does */
    int r2 = arc_line_step(&cl[n & 4 ? 1 : 0], ap[q].x, ap[q].y);
    uint8_t n2 = n >> 3;
    if (r2 >= 1)
        change[q] = 2, control[q] = n2;
    if (r2 == 2)
        draw[q] = n2;
}

static void update_quadrants(void)
{
    memset(change, 0, sizeof change);
    memcpy(draw, control, sizeof draw);
    for (int q = 0; q < 4; q++)
        update_quadrant(q);
}

void plot_arc(void)
{
    gen_arc();
    for (;;) {
        reflect();
        update_quadrants();
        int32_t x = cb.x, y = cb.y;
        if (draw[0] & 1)
            plot_point(ap[0].x, ap[0].y);
        if (x != 0 && (draw[1] & 1))
            plot_point(ap[1].x, ap[1].y);
        if (y != 0) {
            if (draw[3] & 1)
                plot_point(ap[3].x, ap[3].y);
            if (x != 0 && (draw[2] & 1))
                plot_point(ap[2].x, ap[2].y);
        }
        if (cb.x == 0)
            return;
        adv_circle(&cb);
    }
}

/* ---- segments ------------------------------------------------------------------------ */

static struct plot_line *upper_seg, *lower_seg;

static uint32_t double_mul_div_sqrt(uint32_t a, uint32_t b, uint32_t c)
{
    if (!c)
        return 0;                       /* the kernel's division would not end */
    return sqrt16((uint32_t)((uint64_t)a * b / c));
}

/* CompSwapT on two points */
static void compswap(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    if (*y0 < *y1 || (*y0 == *y1 && *x0 >= *x1))
        return;
    int32_t t = *x0;
    *x0 = *x1, *x1 = t;
    t = *y0, *y0 = *y1, *y1 = t;
}

static void gen_seg(void)
{
    int32_t x = cl[1].x, y = cl[1].y;
    uint32_t dx = (uint32_t)cl[1].dx, dy = (uint32_t)cl[1].dy;
    if (vdu.aspect == 1)
        dx <<= 1;
    else if (vdu.aspect > 1)
        dy <<= 1;
    uint32_t dx2 = dx * dx, dy2 = dy * dy, r2 = dx2 + dy2;
    uint32_t iy = double_mul_div_sqrt(dy2, rad_square, r2);
    uint32_t ix = double_mul_div_sqrt(dx2, rad_square, r2);
    if (vdu.aspect == 1)
        ix >>= 1;
    else if (vdu.aspect > 1)
        iy >>= 1;
    x = cl[1].sx >= 0 ? x + (int32_t)ix : x - (int32_t)ix;
    y = cl[1].sy >= 0 ? y + (int32_t)iy : y - (int32_t)iy;
    int32_t x0 = x, y0 = y, x1 = cl[0].ex, y1 = cl[0].ey;
    compswap(&x0, &y0, &x1, &y1);
    uint32_t w = control[0] | (uint32_t)control[1] << 8 | (uint32_t)control[2] << 16 |
                 (uint32_t)control[3] << 24;
    w |= w >> 8;
    w &= w >> 16;
    if (w & 2) {
        plot_gen_line(&cl[2], x0, y0, x1, y1);
        upper_seg = &cl[2];
        plot_gen_line(&cl[3], x1, y1, x0, y0);
        lower_seg = &cl[3];
    } else {
        cl[2].x = x1, cl[2].y = y1;
        cl[3].x = x0, cl[3].y = y0;
        upper_seg = lower_seg = NULL;
    }
}

/* SegLineStep. This gives the chord's run on this row, clamped to
 * [lo, hi]. */
static void seg_line_step(struct plot_line *l, int32_t *lo, int32_t *hi, int32_t row)
{
    int32_t a, b;
    if (l->dy == 0) {
        a = l->ex, b = l->x;
    } else {
        for (uint32_t g = 0; l->y != row && g < GUARD; g++)
            plot_adv_line(l);
        a = l->x;
        if (l->ex != l->x)
            for (uint32_t g = 0; g < GUARD; g++) {
                if (l->bres >= 0) {
                    l->bres -= l->dy;
                    l->x += l->sx;
                }
                if (l->ex == l->x || l->bres < 0)
                    break;
            }
        b = l->x;
    }
    int32_t mn = a < b ? a : b, mx = a < b ? b : a;
    if (mx > *hi) mx = *hi;
    if (mx < *lo) mx = *lo;
    if (mn > *hi) mn = *hi;
    if (mn < *lo) mn = *lo;
    *lo = mn, *hi = mx;
}

static void segment_slice(int32_t x0, int32_t y, int32_t x2, uint8_t dl, uint8_t dr, int32_t s7,
                          int32_t s8)
{
    if (!(dl & 1) && !(dr & 1))
        return;
    if ((dl & 1) && (dr & 1)) {
        plot_hline(x0, y, x2);
        return;
    }
    if (!(dl & 1))
        plot_hline(s7, y, x2);
    else
        plot_hline(x0, y, s8);
}

static uint8_t seg_on(uint8_t sc, int q, struct plot_line *blk, struct plot_line **active)
{
    if (sc == 0 || *active)
        return sc;
    plot_gen_line(blk, ap[q].x, ap[q].y, blk->x, blk->y);
    *active = blk;
    return sc >> 1;
}

void plot_segment(void)
{
    gen_arc();
    gen_seg();
    for (;;) {
        reflect();
        update_quadrants();
        if (change[0] | change[1] | change[2] | change[3]) {
            change[0] = seg_on(change[0], 0, &cl[2], &upper_seg);
            change[1] = seg_on(change[1], 1, &cl[2], &upper_seg);
            change[2] = seg_on(change[2], 2, &cl[3], &lower_seg);
            change[3] = seg_on(change[3], 3, &cl[3], &lower_seg);
        }
        int32_t l = ap[1].x, r = ap[0].x;
        if (upper_seg)
            seg_line_step(upper_seg, &l, &r, ap[1].y);
        segment_slice(ap[1].x, ap[1].y, ap[0].x, draw[1], draw[0], l, r);
        if (cb.y != 0) {
            int32_t l2 = ap[2].x, r2 = ap[3].x;
            if (lower_seg)
                seg_line_step(lower_seg, &l2, &r2, ap[3].y);
            segment_slice(ap[3].x, ap[3].y, ap[2].x, draw[3], draw[2], r2, l2);
        }
        if (change[0] | change[1])
            upper_seg = NULL;
        if (change[2] | change[3])
            lower_seg = NULL;
        do {
            if (cb.x == 0)
                return;
        } while (!adv_circle(&cb));
    }
}

/* ---- sectors ---------------------------------------------------------------------------- */

static void double_hline(int32_t x0, int32_t y, int32_t x2, int32_t a, int32_t b)
{
    plot_hline(x0, y, a);
    plot_hline(b, y, x2);
}

static void sector_slice(int32_t x0, int32_t y, int32_t x2, uint8_t dl, uint8_t dr)
{
    if (dr == 0x57) {
        double_hline(x0, y, x2, cl[0].x, cl[1].ex);
        return;
    }
    if (dl == 0x73) {
        double_hline(x0, y, x2, cl[0].ex, cl[1].x);
        return;
    }
    if (dr == 0x07 && dl == 0x03) {
        double_hline(x0, y, x2, cl[0].ex, cl[1].ex);
        return;
    }
    if (dr == 0x07) {
        plot_hline(cl[1].ex, y, x2);
        return;
    }
    if (dl == 0x03) {
        plot_hline(x0, y, cl[0].ex);
        return;
    }
    if (dr == 0x3A) {
        plot_hline(cl[1].ex, y, cl[0].x);
        return;
    }
    if (dl == 0x1E) {
        plot_hline(cl[1].x, y, cl[0].ex);
        return;
    }
    if ((int8_t)dr < 1)
        return;
    if (dr > 1)
        x2 = cl[0].x;
    if (dl > 1)
        x0 = cl[1].x;
    plot_hline(x0, y, x2);
}

void plot_sector(void)
{
    gen_arc();
    for (;;) {
        reflect();
        update_quadrants();
        if (cb.y == 0) {
            int32_t n0 = cl[0].ex, f0 = cl[0].x, f1 = cl[1].x;
            int32_t r = n0 > f0 ? n0 : f0, l = n0 < f0 ? n0 : f0;
            if (f1 > r) r = f1;
            if (f1 < l) l = f1;
            if ((draw[0] & 1) || (draw[3] & 1))
                r = ap[0].x;
            if ((draw[1] & 1) || (draw[2] & 1))
                l = ap[1].x;
            plot_new_hline(l, ap[0].y, r);
        } else {
            sector_slice(ap[1].x, ap[1].y, ap[0].x, draw[1], draw[0]);
            sector_slice(ap[3].x, ap[3].y, ap[2].x, draw[3], draw[2]);
        }
        do {
            if (cb.x == 0)
                return;
        } while (!adv_circle(&cb));
    }
}

/* ---- ellipses ----------------------------------------------------------------------------- */

static struct {
    int32_t slice_cnt, odd, xoffset, y, shear, slice_x;
    uint32_t ysqr, max_ysqr;
    int32_t cx, cy;
    int32_t this_l, this_r, next_l, next_r;
} el;

/* AdvEllP20. This gives the span of the row whose values are current. */
static void ell_slice(int32_t *l, int32_t *r)
{
    uint32_t s = sqrt_alt(el.max_ysqr - el.ysqr, 24);
    int32_t w = (int32_t)((uint32_t)el.slice_x * s);
    *r = (int32_t)((uint32_t)el.xoffset + (uint32_t)w + 0x8000u) >> 16;
    *l = (int32_t)((uint32_t)el.xoffset - (uint32_t)w + 0x8000u) >> 16;
    el.ysqr += (uint32_t)el.odd;
    el.odd += 2;
    el.xoffset += el.shear;
    el.slice_cnt -= 1;
    el.y += 1;
}

static void ell_hline(int32_t l, int32_t k, int32_t r)
{
    plot_new_hline(el.cx + l, el.cy + k, el.cx + r);
    if (k != 0)
        plot_new_hline(el.cx - r, el.cy - k, el.cx - l);
}

void plot_ellipse(int fill)
{
    int32_t cx = vdu.oldx, cy = vdu.oldy;
    int32_t w = vdu.gcsix - cx, dy = vdu.newpty - cy, dxs = vdu.newptx - cx;
    if (w < 0)
        w = -w;
    if (dy == 0) {
        plot_new_hline(cx - w, cy, cx + w);
        return;
    }
    int32_t h = dy < 0 ? -dy : dy, s = dxs < 0 ? -dxs : dxs;
    el.cx = cx, el.cy = cy;
    el.slice_cnt = h;
    el.shear = (int32_t)(((uint32_t)s << 16) / (uint32_t)h);
    if ((dy ^ dxs) < 0)
        el.shear = -el.shear;
    el.slice_x = (int32_t)(((uint32_t)w << 8) / (uint32_t)h);
    el.max_ysqr = (uint32_t)h * (uint32_t)h;
    el.ysqr = 0, el.odd = 1, el.xoffset = 0, el.y = -2;
    int32_t tl, tr, nl, nr;
    ell_slice(&tl, &tr);
    ell_slice(&nl, &nr);
    int32_t pl = -nr, pr = -nl;
    if (pr < tl) tl = pr;
    if (nr < tl) tl = nr;
    if (pl > tr) tr = pl;
    if (nl > tr) tr = nl;
    el.this_l = tl, el.this_r = tr, el.next_l = nl, el.next_r = nr;

    for (;;) {
        if (fill) {
            ell_hline(el.this_l, el.y, el.this_r);
        } else {
            int32_t mx = pl > el.next_l ? pl : el.next_l, mn = pr < el.next_r ? pr : el.next_r;
            int32_t lend = el.this_l > mx - 1 ? el.this_l : mx - 1;
            int32_t rstart = el.this_r < mn + 1 ? el.this_r : mn + 1;
            if (lend >= rstart) {
                ell_hline(el.this_l, el.y, el.this_r);
            } else {
                ell_hline(el.this_l, el.y, lend);
                ell_hline(rstart, el.y, el.this_r);
            }
        }
        if (el.slice_cnt < 0)
            break;
        /* AdvEllParm */
        int32_t al, ar;
        ell_slice(&al, &ar);
        pl = el.this_l, pr = el.this_r;
        int32_t thl = el.next_l, thr = el.next_r;
        if (al > thr) thr = al;
        if (ar < thl) thl = ar;
        el.this_l = thl, el.this_r = thr, el.next_l = al, el.next_r = ar;
    }
    ell_hline(el.next_l, el.y + 1, el.next_r);
}
