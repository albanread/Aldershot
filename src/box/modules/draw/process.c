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
 * (Sources/Video/Render/DrawMod: s.DrProcess).
 */

/* process.c: the processing stages (s/DrProcess). They are floating point
 * (not implemented, as in the original), transforming by a matrix,
 * flattening Beziers, splitting long edges, thickening with joins and caps,
 * thin strokes, and dashing.
 *
 * Each is the original's routine, transliterated. Its arithmetic is 32-bit
 * and wrapping where the original's is, and double precision where the
 * original's is. One quirk of the original is kept. On a miss in the
 * transform cache, the current point is transformed but the result is
 * discarded (a MOVNE after a call that returns EQ). So the edge goes down
 * the chain with its old end untransformed.
 */
#include "draw.h"

static int32_t avg(int32_t a, int32_t b)        /* the original's SignedAverage */
{
    return (int32_t)(((int64_t)a + b) >> 1);
}

/* ---- floating point -------------------------------------------------------------------- */

os_error *process_float(struct draw *d, int i, struct draw_el *e)
{
    (void)d, (void)i, (void)e;
    return draw_err(DE_UNIMPLEMENTED);
}

/* ---- transform ------------------------------------------------------------------------- */

/* process_transform_point: multiply by the 16.16 matrix, rounding to nearest
 * (the bit below the binary point is carried in), then add the translation.
 * Overflow is an error. */
static os_error *transform_point(struct draw *d, int32_t *x, int32_t *y)
{
    int32_t a = (int32_t)ros_ld32(d->matrix), b = (int32_t)ros_ld32(d->matrix + 4);
    int32_t c = (int32_t)ros_ld32(d->matrix + 8), dd = (int32_t)ros_ld32(d->matrix + 12);
    int32_t ee = (int32_t)ros_ld32(d->matrix + 16), f = (int32_t)ros_ld32(d->matrix + 20);
    int64_t sx, sy;
    if (__builtin_add_overflow((int64_t)a * *x, (int64_t)c * *y, &sx) ||
        __builtin_add_overflow((int64_t)b * *x, (int64_t)dd * *y, &sy))
        return draw_err(DE_OVERFLOW);
    int64_t rx = (sx >> 16) + ((sx >> 15) & 1) + ee;
    int64_t ry = (sy >> 16) + ((sy >> 15) & 1) + f;
    if (rx != (int32_t)rx || ry != (int32_t)ry)
        return draw_err(DE_OVERFLOW);
    *x = (int32_t)rx, *y = (int32_t)ry;
    return NULL;
}

os_error *process_transform(struct draw *d, int i, struct draw_el *e)
{
    int32_t nx = e->x, ny = e->y;               /* the new point as it came in (the original pushes it) */
    os_error *err;
    switch (e->type) {
    case ET_END:
        err = draw_next(d, i, e);
        if (!err)
            e->y0 = nx;
        return err;
    case ET_START: {
        d->before[0] = d->before[1] = 0;
        d->after[0] = (int32_t)ros_ld32(d->matrix + 16);
        d->after[1] = (int32_t)ros_ld32(d->matrix + 20);
        int32_t a = (int32_t)ros_ld32(d->matrix), b = (int32_t)ros_ld32(d->matrix + 4);
        int32_t c = (int32_t)ros_ld32(d->matrix + 8), dd = (int32_t)ros_ld32(d->matrix + 12);
        if ((__int128)a * dd - (__int128)b * c < 0)
            d->userspacewinding = -1;
        break;
    }
    case ET_BEZIER:
        if ((err = transform_point(d, &e->c1x, &e->c1y)) ||
            (err = transform_point(d, &e->c2x, &e->c2y)))
            return err;
        /* fall through */
    case ET_CLOSEGAP: case ET_CLOSELINE: case ET_GAP: case ET_LINE:
        if (e->x0 == d->before[0] && e->y0 == d->before[1]) {
            e->x0 = d->after[0], e->y0 = d->after[1];
        } else {
            int32_t ox = e->x0, oy = e->y0;     /* (the result is not kept) */
            if ((err = transform_point(d, &ox, &oy)))
                return err;
        }
        /* fall through */
    case ET_MOVE: case ET_SPECIALMOVE:
        d->before[0] = e->x, d->before[1] = e->y;
        if ((err = transform_point(d, &e->x, &e->y)))
            return err;
        d->after[0] = e->x, d->after[1] = e->y;
        break;
    }
    err = draw_next(d, i, e);
    if (!err)
        e->x0 = nx, e->y0 = ny;
    return err;
}

/* ---- flatten ----------------------------------------------------------------------------- */

static os_error *flatten_bezier(struct draw *d, int i, struct draw_el *e)
{
    for (;;) {
        int bisect = 0;
        int32_t lo = e->x0, hi = e->x0;
        if (lo > e->c1x) lo = e->c1x;
        if (hi < e->c1x) hi = e->c1x;
        if (lo > e->c2x) lo = e->c2x;
        if (hi < e->c2x) hi = e->c2x;
        if (lo > e->x) lo = e->x;
        if (hi < e->x) hi = e->x;
        if ((uint32_t)hi - (uint32_t)lo > 0x2A000000u)
            bisect = 1;
        if (!bisect) {
            lo = hi = e->y0;
            if (lo > e->c1y) lo = e->c1y;
            if (hi < e->c1y) hi = e->c1y;
            if (lo > e->c2y) lo = e->c2y;
            if (hi < e->c2y) hi = e->c2y;
            if (lo > e->y) lo = e->y;
            if (hi < e->y) hi = e->y;
            if ((uint32_t)hi - (uint32_t)lo > 0x2A000000u)
                bisect = 1;
        }
        for (int k = 0; k < 2 && !bisect; k++) {
            /* The sum of the absolute values of 3*c1 - 2*p0 - p3, taken in x and in
             * y. The second pass does the same with c2, p3 and p0 swapped. */
            int32_t cx = k ? e->c2x : e->c1x, cy = k ? e->c2y : e->c1y;
            int32_t px = k ? e->x : e->x0, py = k ? e->y : e->y0;
            int32_t qx = k ? e->x0 : e->x, qy = k ? e->y0 : e->y;
            uint32_t m = (uint32_t)cx * 3 - ((uint32_t)px << 1) - (uint32_t)qx;
            if ((int32_t)m < 0)
                m = 0u - m;
            uint32_t n = (uint32_t)cy * 3 - ((uint32_t)py << 1) - (uint32_t)qy;
            m = (int32_t)n >= 0 ? m + n : m - n;
            if ((int32_t)m > d->flatlimit)
                bisect = 1;
        }
        if (!bisect) {
            e->type = ET_LINE;
            return draw_next(d, i, e);
        }
        int32_t p3x = e->x, p3y = e->y;
        int32_t m23x = avg(e->c2x, e->x), m23y = avg(e->c2y, e->y);
        int32_t m12x = avg(e->c1x, e->c2x), m12y = avg(e->c1y, e->c2y);
        int32_t m123x = avg(m12x, m23x), m123y = avg(m12y, m23y);
        int32_t m01x = avg(e->x0, e->c1x), m01y = avg(e->y0, e->c1y);
        int32_t m012x = avg(m01x, m12x), m012y = avg(m01y, m12y);
        e->c1x = m01x, e->c1y = m01y, e->c2x = m012x, e->c2y = m012y;
        e->x = avg(m012x, m123x), e->y = avg(m012y, m123y);
        os_error *err = flatten_bezier(d, i, e);
        if (err)
            return err;
        e->type = ET_BEZIER;
        e->c1x = m123x, e->c1y = m123y, e->c2x = m23x, e->c2y = m23y;
        e->x = p3x, e->y = p3y;
    }
}

os_error *process_flatten(struct draw *d, int i, struct draw_el *e)
{
    if (e->type == ET_BEZIER)
        return flatten_bezier(d, i, e);
    if (e->type == ET_START) {
        uint32_t f = (uint32_t)d->flatness << 2;
        d->flatlimit = f ? (int32_t)f : 2 << 10;
    }
    return draw_next(d, i, e);
}

/* ---- long edges -------------------------------------------------------------------------- */

static os_error *longedge(struct draw *d, int i, struct draw_el *e)
{
    for (;;) {
        int64_t dx = (int64_t)e->x - e->x0, dy = (int64_t)e->y - e->y0;
        uint32_t t1 = (uint32_t)(dx < 0 ? -dx : dx), t2 = (uint32_t)(dy < 0 ? -dy : dy);
        if (t1 < 0x20000000u && t2 < 0x20000000u)
            return draw_next(d, i, e);
        int32_t nx = e->x, ny = e->y;
        uint32_t type = e->type;
        e->x = avg(e->x0, e->x), e->y = avg(e->y0, e->y);
        if (e->type <= ET_CLOSELINE)
            e->type += ET_GAP - ET_CLOSEGAP;
        os_error *err = longedge(d, i, e);
        if (err)
            return err;
        e->x = nx, e->y = ny, e->type = type;
    }
}

os_error *process_longedgeprotect(struct draw *d, int i, struct draw_el *e)
{
    if (e->type != ET_BEZIER && e->type > ET_SPECIALMOVE)
        return longedge(d, i, e);
    return draw_next(d, i, e);
}

/* ---- thicken ------------------------------------------------------------------------------ */

enum { JC_EXISTS = 1, JC_STARTED = 2, JC_INITIALVALID = 4, JC_FINALVALID = 8 };

/* measurevector: the length of a vector. */
static uint32_t measurevector(int32_t t1, int32_t t2)
{
    uint32_t a = (uint32_t)t2, b = (uint32_t)t1;
    if ((int32_t)a < 0)
        a = 0u - a;
    if ((int32_t)b < 0)
        b = 0u - b;
    if (a <= 0xB500 && b <= 0xB500) {
        uint32_t v = a * a + b * b, bit = 0x40000000, r = 0;
        do {
            r += bit;
            if (v >= r) {
                v -= r;
                r += bit;
            } else {
                r -= bit;
            }
            r >>= 1;
            bit >>= 2;
        } while (bit);
        return r;
    }
    uint64_t s = (uint64_t)((int64_t)(int32_t)a * (int32_t)a) +
                 (uint64_t)((int64_t)(int32_t)b * (int32_t)b);
    return draw_dsqrts((uint32_t)s, (uint32_t)(s >> 32));
}

/* measurealongvector: scale (t1, t2) to the length |t3|. */
static void measurealong(int32_t *t1p, int32_t *t2p, int32_t t3)
{
    int32_t t1 = *t1p, t2 = *t2p;
    if (t2 == 0) {
        if (t1 > 0)
            *t1p = t3;
        else if (t1 < 0)
            *t1p = (int32_t)(0u - (uint32_t)t3);
        return;
    }
    if (t1 == 0) {
        if (t2 > 0)
            *t2p = t3;
        else if (t2 < 0)
            *t2p = (int32_t)(0u - (uint32_t)t3);
        return;
    }
    if (t3 == 0) {
        *t1p = *t2p = 0;
        return;
    }
    uint32_t len = (uint32_t)t3;
    if (t3 < 0) {
        len = 0u - len;
        if ((int32_t)len < 0)
            len = 0x7FFFFFFF;
    }
    int signs = 0;
    uint32_t a = (uint32_t)t1, b = (uint32_t)t2;
    if (t1 < 0)
        signs |= 1, a = 0u - a;
    if (t2 < 0)
        signs |= 2, b = 0u - b;
    while (a >= len << 1 || b >= len << 1)
        a >>= 1, b >>= 1;
    uint32_t l = measurevector((int32_t)a, (int32_t)b), ra, rb;
    if (a <= 0x10000 && b <= 0x10000 && len <= 0x10000) {
        ra = len * a / l;
        rb = len * b / l;
    } else {
        uint64_t pa = (uint64_t)((int64_t)(int32_t)a * (int32_t)len);
        uint64_t pb = (uint64_t)((int64_t)(int32_t)b * (int32_t)len);
        ra = draw_dsdivs((uint32_t)pa, (uint32_t)(pa >> 32), l);
        rb = draw_dsdivs((uint32_t)pb, (uint32_t)(pb >> 32), l);
    }
    if (signs & 1)
        ra = 0u - ra;
    if (signs & 2)
        rb = 0u - rb;
    *t1p = (int32_t)ra, *t2p = (int32_t)rb;
}

/* Emit a closed polygon. It is a move to p[0], lines to p[1..n-1], and a
 * close with a line back to p[n]. (The original does this with pushes and
 * pulls.) */
static os_error *polygon(struct draw *d, int i, struct draw_el *e, const int32_t (*p)[2], int n)
{
    for (int k = 0; k <= n; k++) {
        e->type = k == 0 ? ET_MOVE : k == n ? ET_CLOSELINE : ET_LINE;
        e->x = p[k][0], e->y = p[k][1];
        os_error *err = draw_next(d, i, e);
        if (err)
            return err;
    }
    return NULL;
}

static os_error *drawcircle(struct draw *d, int i, struct draw_el *e, int32_t ox, int32_t oy)
{
    int32_t r = d->thickness >> 1, cc = d->circlecontrol;
    os_error *err;
    e->x0 = ox, e->y0 = oy;
    e->type = ET_MOVE;
    e->x = ox + r, e->y = oy;
    if ((err = draw_next(d, i, e)))
        return err;
    for (int q = 0; q < 4; q++) {
        int32_t x0 = e->x0, y0 = e->y0;
        e->type = ET_BEZIER;
        switch (q) {
        case 0:
            e->x = x0 - r, e->y = y0 + r;
            e->c1x = x0, e->c1y = y0 + cc, e->c2x = e->x + cc, e->c2y = e->y;
            break;
        case 1:
            e->x = x0 - r, e->y = y0 - r;
            e->c1x = x0 - cc, e->c1y = y0, e->c2x = e->x, e->c2y = e->y + cc;
            break;
        case 2:
            e->x = x0 + r, e->y = y0 - r;
            e->c1x = x0, e->c1y = y0 - cc, e->c2x = e->x - cc, e->c2y = e->y;
            break;
        default:
            e->x = x0 + r, e->y = y0 + r;
            e->c1x = x0 + cc, e->c1y = y0, e->c2x = e->x, e->c2y = e->y - cc;
            break;
        }
        if ((err = draw_next(d, i, e)))
            return err;
    }
    e->type = ET_CLOSELINE;
    e->x = e->x0, e->y = e->y0;
    return draw_next(d, i, e);
}

/* process_findopenside: if (t3,t4) is clockwise of (t1,t2), swap the two
 * vectors and negate them. */
static void findopenside(int32_t t[4])
{
    int ge;
    if ((uint32_t)t[0] + 0xB400 <= 2 * 0xB400 && (uint32_t)t[1] + 0xB400 <= 2 * 0xB400 &&
        (uint32_t)t[2] + 0xB400 <= 2 * 0xB400 && (uint32_t)t[3] + 0xB400 <= 2 * 0xB400)
        ge = (int32_t)((uint32_t)t[0] * (uint32_t)t[3]) >= (int32_t)((uint32_t)t[1] * (uint32_t)t[2]);
    else
        ge = (int64_t)t[0] * t[3] - (int64_t)t[1] * t[2] >= 0;
    if (!ge) {
        int32_t a = t[0], b = t[1];
        t[0] = (int32_t)(0u - (uint32_t)t[2]), t[1] = (int32_t)(0u - (uint32_t)t[3]);
        t[2] = (int32_t)(0u - (uint32_t)a), t[3] = (int32_t)(0u - (uint32_t)b);
    }
}

/* All the caps and joins leave the current point where it was. */
static os_error *keep(struct draw_el *e, int32_t x0, int32_t y0, os_error *err)
{
    if (!err)
        e->x0 = x0, e->y0 = y0;
    return err;
}

/* process_drawcap: which is 1 for a leading cap and 2 for a trailing cap.
 * (t1, t2) is the cap's offset. */
static os_error *drawcap(struct draw *d, int i, struct draw_el *e, int which, int32_t t1, int32_t t2)
{
    int32_t ox = e->x0, oy = e->y0;
    d->currentoffset[0] = t1, d->currentoffset[1] = t2;
    uint32_t cap = ros_ld8(d->joinsandcaps + (uint32_t)which);
    switch (cap) {
    case 0:
        return NULL;
    case 1:
        return keep(e, ox, oy, drawcircle(d, i, e, ox, oy));
    case 2: {                                   /* square: five vertices */
        int32_t t3 = ox - t1, t4 = oy - t2;
        int32_t c2x = t3 - t2, c2y = t4 + t1;
        int32_t p[6][2] = { { ox, oy }, { ox + t1, oy + t2 }, { c2x + 2 * t1, c2y + 2 * t2 },
                            { c2x, c2y }, { t3, t4 }, { ox, oy } };
        return keep(e, ox, oy, polygon(d, i, e, p, 5));
    }
    case 3: {                                   /* triangle: six vertices */
        uint32_t param = ros_ld32(d->joinsandcaps + 4 * (uint32_t)(which + 1));
        int32_t lm = (int32_t)(param >> 16), wm = (int32_t)(param & 0xFFFF);
        int32_t ax = (int32_t)(uint32_t)(((int64_t)lm * t2) >> 7);
        int32_t ay = (int32_t)(uint32_t)(((int64_t)lm * t1) >> 7);
        int32_t wx = (int32_t)(uint32_t)(((int64_t)wm * t1) >> 7);
        int32_t wy = (int32_t)(uint32_t)(((int64_t)wm * t2) >> 7);
        int32_t p[7][2] = { { ox + t1, oy + t2 }, { ox + wx, oy + wy }, { ox - ax, oy + ay },
                            { ox - wx, oy - wy }, { ox - t1, oy - t2 }, { ox, oy },
                            { ox + t1, oy + t2 } };
        return keep(e, ox, oy, polygon(d, i, e, p, 6));
    }
    default:
        return draw_err(DE_CAPSJOINS);
    }
}

static os_error *drawleadingcap(struct draw *d, int i, struct draw_el *e)
{
    return drawcap(d, i, e, 1, (int32_t)(0u - (uint32_t)d->finaloffset[0]),
                   (int32_t)(0u - (uint32_t)d->finaloffset[1]));
}

static os_error *drawonlycap(struct draw *d, int i, struct draw_el *e)
{
    int32_t ox = e->x0, oy = e->y0;
    if (ros_ld8(d->joinsandcaps + 1) == 1 && ros_ld8(d->joinsandcaps + 2) == 1)
        return keep(e, ox, oy, drawcircle(d, i, e, ox, oy));
    return NULL;
}

/* process_drawjoin at the current point. (t1, t2) is the new segment's
 * offset and finaloffset is the old segment's. */
static os_error *drawjoin(struct draw *d, int i, struct draw_el *e, int32_t t1, int32_t t2)
{
    int32_t vx = e->x0, vy = e->y0;
    int32_t t[4] = { t1, t2, d->finaloffset[0], d->finaloffset[1] };
    uint32_t type = ros_ld8(d->joinsandcaps);
    if (type >= 3)
        return draw_err(DE_CAPSJOINS);
    if (type == 1)
        return keep(e, vx, vy, drawcircle(d, i, e, vx, vy));
    findopenside(t);
    if (type == 0) {                            /* mitred */
        int32_t limit = (int32_t)ros_ld32(d->joinsandcaps + 4);
        if (limit < 0x10000)
            return draw_err(DE_CAPSJOINS);
        int32_t sx = (int32_t)((uint32_t)t[0] + (uint32_t)t[2]);
        int32_t sy = (int32_t)((uint32_t)t[1] + (uint32_t)t[3]);
        uint64_t sum = (uint64_t)((int64_t)sx * sx) + (uint64_t)((int64_t)sy * sy);
        uint64_t th2 = (uint64_t)((int64_t)d->thickness * d->thickness);
        uint32_t qlo, qhi;
        uint64_t v = th2 << 16;
        draw_dsdivd((uint32_t)v, (uint32_t)(v >> 32), (uint32_t)limit, &qlo, &qhi);
        v = ((uint64_t)qhi << 32 | qlo) << 16;
        draw_dsdivd((uint32_t)v, (uint32_t)(v >> 32), (uint32_t)limit, &qlo, &qhi);
        if (((uint64_t)qhi << 32 | qlo) < sum) {
            uint32_t ylo, yhi, xlo, xhi;
            draw_dsmultd((uint32_t)th2, (uint32_t)(th2 >> 32), sy, &ylo, &yhi);
            draw_dsmultd((uint32_t)th2, (uint32_t)(th2 >> 32), sx, &xlo, &xhi);
            int32_t mx = draw_dddivs(xlo, xhi, (uint32_t)sum, (uint32_t)(sum >> 32)) >> 1;
            int32_t my = draw_dddivs(ylo, yhi, (uint32_t)sum, (uint32_t)(sum >> 32)) >> 1;
            int32_t p[5][2] = { { vx, vy }, { vx + t[0], vy + t[1] }, { vx + mx, vy + my },
                                { vx + t[2], vy + t[3] }, { vx, vy } };
            return keep(e, vx, vy, polygon(d, i, e, p, 4));
        }
    }
    int32_t p[4][2] = { { vx, vy }, { vx + t[0], vy + t[1] }, { vx + t[2], vy + t[3] },
                        { vx, vy } };           /* bevelled */
    return keep(e, vx, vy, polygon(d, i, e, p, 3));
}

static os_error *unclosedjoin(struct draw *d, int i, struct draw_el *e)
{
    uint32_t t3 = d->joincapstate;
    d->joincapstate = 0;
    if (!(t3 & JC_EXISTS))
        return NULL;
    if (!(t3 & JC_STARTED))
        return drawonlycap(d, i, e);
    if (t3 & JC_INITIALVALID) {
        int32_t ox = e->x0, oy = e->y0;
        e->x0 = d->initialvertex[0], e->y0 = d->initialvertex[1];
        os_error *err = drawcap(d, i, e, 2, d->initialoffset[0], d->initialoffset[1]);
        if (err)
            return err;
        e->x0 = ox, e->y0 = oy;
    }
    if (t3 & JC_FINALVALID)
        return drawleadingcap(d, i, e);
    return NULL;
}

static os_error *closedjoin(struct draw *d, int i, struct draw_el *e)
{
    uint32_t t3 = d->joincapstate;
    d->joincapstate = 0;
    if (!(t3 & JC_EXISTS))
        return NULL;
    if (!(t3 & JC_STARTED))
        return drawonlycap(d, i, e);
    if (t3 & JC_INITIALVALID) {
        if (t3 & JC_FINALVALID)
            return drawjoin(d, i, e, d->initialoffset[0], d->initialoffset[1]);
        return drawcap(d, i, e, 2, d->initialoffset[0], d->initialoffset[1]);
    }
    if (t3 & JC_FINALVALID)
        return drawleadingcap(d, i, e);
    return NULL;
}

/* process_thickengap */
static os_error *thickengap(struct draw *d, int i, struct draw_el *e)
{
    int32_t nx = e->x, ny = e->y;
    uint32_t type = d->joincapstate;
    os_error *err = NULL;
    if (!(type & JC_STARTED))
        type |= JC_STARTED;
    else if (type & JC_FINALVALID)
        err = drawleadingcap(d, i, e);
    d->joincapstate = type & ~(uint32_t)JC_FINALVALID;
    if (!err)
        e->x0 = nx, e->y0 = ny;
    return err;
}

/* process_thickensegment: draw a join or cap at the current point, then the
 * hexagon around the segment. */
static os_error *thickensegment(struct draw *d, int i, struct draw_el *e)
{
    int32_t ox = e->x0, oy = e->y0, nx = e->x, ny = e->y;
    int32_t t1 = (int32_t)((uint32_t)oy - (uint32_t)ny), t2 = (int32_t)((uint32_t)nx - (uint32_t)ox);
    if ((t1 | t2) == 0)
        return NULL;
    measurealong(&t1, &t2, d->thickness);
    t1 >>= 1, t2 >>= 1;
    uint32_t type = d->joincapstate;
    if (!(type & JC_STARTED)) {
        d->initialvertex[0] = ox, d->initialvertex[1] = oy;
        d->initialoffset[0] = t1, d->initialoffset[1] = t2;
        type |= JC_STARTED | JC_INITIALVALID;
    } else {
        os_error *err = type & JC_FINALVALID ? drawjoin(d, i, e, t1, t2)
                                             : drawcap(d, i, e, 2, t1, t2);
        if (err)
            return err;
    }
    d->finaloffset[0] = t1, d->finaloffset[1] = t2;
    d->joincapstate = type | JC_FINALVALID;
    int32_t p[7][2] = { { nx, ny }, { nx + t1, ny + t2 }, { ox + t1, oy + t2 }, { ox, oy },
                        { ox - t1, oy - t2 }, { nx - t1, ny - t2 }, { nx, ny } };
    return polygon(d, i, e, p, 6);
}

os_error *process_thicken(struct draw *d, int i, struct draw_el *e)
{
    os_error *err;
    switch (e->type) {
    case ET_END:
        if ((err = unclosedjoin(d, i, e)))
            return err;
        e->type = ET_END;
        return draw_next(d, i, e);
    case ET_START: {
        int32_t t = d->thickness;
        uint32_t lo = (uint32_t)t & 0xFFFF;
        d->circlecontrol = (int32_t)(0x46B1u * (uint32_t)(t >> 16) + ((0x46B1u * lo) >> 16));
        d->joincapstate = 0;
        return draw_next(d, i, e);
    }
    case ET_MOVE: case ET_SPECIALMOVE: {
        int32_t nx = e->x, ny = e->y;
        err = unclosedjoin(d, i, e);
        d->joincapstate |= JC_EXISTS;
        e->x0 = nx, e->y0 = ny;
        return err;
    }
    case ET_CLOSEGAP: {
        int32_t nx = e->x, ny = e->y;
        err = unclosedjoin(d, i, e);
        e->x0 = nx, e->y0 = ny;
        return err;
    }
    case ET_CLOSELINE:
        if ((err = thickensegment(d, i, e)))
            return err;
        return closedjoin(d, i, e);
    case ET_BEZIER:
        return draw_err(DE_NOTFLAT);
    case ET_GAP:
        return thickengap(d, i, e);
    default:
        return thickensegment(d, i, e);
    }
}

os_error *process_zerothicken(struct draw *d, int i, struct draw_el *e)
{
    if (e->type == ET_MOVE)
        e->type = ET_SPECIALMOVE;
    return draw_next(d, i, e);
}

/* ---- dash -------------------------------------------------------------------------------- */

/* process_advancedash: move t4 further along the dash pattern. */
static void advancedash(struct draw *d, uint32_t t1, uint32_t t2, uint32_t t3, uint32_t t4)
{
    for (;;) {
        uint32_t left = t3 - t4;
        if (t3 > t4) {
            t3 = left;
            break;
        }
        t4 = 0u - left;
        t1 ^= ET_LINE ^ ET_GAP;
        t2++;
        if (t2 >= ros_ld32(d->dashptr - 4))
            t2 = 0;
        t3 = ros_ld32(d->dashptr + 4 * t2);
    }
    d->dashstate = t1, d->dashindex = t2, d->dashdistance = t3;
}

os_error *process_dash(struct draw *d, int i, struct draw_el *e)
{
    switch (e->type) {
    case ET_START:
        d->dashptr += 8;
        break;
    case ET_MOVE: case ET_SPECIALMOVE:
        advancedash(d, ET_LINE, 0, ros_ld32(d->dashptr), ros_ld32(d->dashptr - 8));
        break;
    case ET_BEZIER:
        return draw_err(DE_NOTFLAT);
    case ET_GAP:
        advancedash(d, d->dashstate, d->dashindex, d->dashdistance,
                    measurevector((int32_t)((uint32_t)e->x - (uint32_t)e->x0),
                                  (int32_t)((uint32_t)e->y - (uint32_t)e->y0)));
        break;
    case ET_CLOSELINE: case ET_LINE:
        for (;;) {
            int32_t nx = e->x, ny = e->y;
            uint32_t type = e->type;
            int32_t dx = (int32_t)((uint32_t)nx - (uint32_t)e->x0);
            int32_t dy = (int32_t)((uint32_t)ny - (uint32_t)e->y0);
            uint32_t t4 = measurevector(dx, dy);
            uint32_t t1 = d->dashstate, t2 = d->dashindex, t3 = d->dashdistance;
            if (t3 > t4) {
                d->dashdistance = t3 - t4;
                e->type = type > ET_CLOSELINE ? t1 : t1 - (ET_GAP - ET_CLOSEGAP);
                return draw_next(d, i, e);
            }
            uint32_t s = t1 ^ (ET_LINE ^ ET_GAP), k = t2 + 1;
            if (k >= ros_ld32(d->dashptr - 4))
                k = 0;
            d->dashstate = s, d->dashindex = k, d->dashdistance = ros_ld32(d->dashptr + 4 * k);
            measurealong(&dx, &dy, (int32_t)t3);
            e->type = t1;
            e->x = (int32_t)((uint32_t)dx + (uint32_t)e->x0);
            e->y = (int32_t)((uint32_t)dy + (uint32_t)e->y0);
            os_error *err = draw_next(d, i, e);
            if (err)
                return err;
            e->x = nx, e->y = ny, e->type = type;
        }
    default:
        break;
    }
    return draw_next(d, i, e);
}
