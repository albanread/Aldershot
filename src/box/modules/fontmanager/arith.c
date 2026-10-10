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
 * (Sources/Video/Render/Fonts/FontManager: s.Font_Arith, s.Font_BasFP).
 */

/* arith.c: the Font Manager's arithmetic.  This is its own floating point
 * (s/Font_BasFP, which is BBC BASIC's, used to set up matrices) and its
 * matrices (s/Font_Arith).
 *
 * A float is a 32-bit mantissa, a biased exponent and a sign.  The
 * mantissa is normalised (bit 31 set) or 0.  The value is
 * m * 2^(e - 128 - 32), so 1.0 is (&80000000, &81).  In a matrix a float
 * is two words: the mantissa, and the exponent with the sign in bit 31.
 * Every routine does what the original does, rounding included. This
 * makes the matrices, and the widths, bounding boxes and sizes derived
 * from them, the same as the original's to the last bit.
 *
 * A fixed matrix holds XX, YX, XY and YY in 16.16, shifted down by its
 * coordshift. Then come the translation X and Y as integers.  Points are
 * transformed as rows: (x, y) -> (x*XX + y*XY + X, x*YX + y*YY + Y).
 */
#include <string.h>

#include "fm.h"

typedef struct {
    uint32_t m;
    int32_t e;
    uint32_t s;
} bf;

static const bf ZERO = { 0, 0, 0 };

static bf unpack(struct fm_fp v)
{
    return (bf){ v.m, (int32_t)(v.w & 255), v.w & 0x80000000u };
}

static struct fm_fp pack(bf a)
{
    return (struct fm_fp){ a.m, (uint32_t)a.e | a.s };
}

static int clz32(uint32_t v)
{
    return v ? __builtin_clz(v) : 32;
}

/* IFLT */
static bf iflt(int32_t v)
{
    if (v == 0)
        return ZERO;
    bf r;
    r.s = (uint32_t)v & 0x80000000u;
    uint32_t m = r.s ? 0u - (uint32_t)v : (uint32_t)v;
    int n = clz32(m);
    r.m = m << n;
    r.e = 0xA0 - n;
    return r;
}

/* INTRND: to the nearest integer, halves away from zero */
static os_error *intrnd(bf a, int32_t *out)
{
    int32_t e = a.e - 0x80;
    if (e < 0) {
        *out = 0;
        return NULL;
    }
    uint32_t g = e >= 32 ? 0 : a.m << e;
    int32_t sh = 32 - e;
    if (sh <= 0)
        return fm_err(FE_OVERFLOW, NULL, NULL);
    uint32_t r = sh >= 32 ? 0 : a.m >> sh;
    if (g & 0x80000000u)
        r++;
    *out = a.s ? -(int32_t)r : (int32_t)r;
    return NULL;
}

/* FMUL: a * w.  A tie is rounded to odd. */
static os_error *fmul(bf a, bf w, bf *out)
{
    if (a.m == 0 || w.m == 0) {
        *out = ZERO;
        return NULL;
    }
    bf r;
    r.s = a.s ^ w.s;
    r.e = a.e + w.e - 0x80;
    uint64_t p = (uint64_t)a.m * w.m;
    uint32_t hi = (uint32_t)(p >> 32), lo = (uint32_t)p;
    if (!(hi & 0x80000000u)) {
        hi = hi << 1 | lo >> 31;
        lo <<= 1;
        r.e--;
    }
    if (lo >= 0x80000000u) {
        if (lo == 0x80000000u)
            hi &= ~1u;
        if (++hi == 0) {
            hi = 0x80000000u;
            r.e++;
        }
    }
    r.m = hi;
    if ((uint32_t)r.e < 256) {
        *out = r;
        return NULL;
    }
    if (r.e < 0) {
        *out = ZERO;
        return NULL;
    }
    return fm_err(FE_OVERFLOW, NULL, NULL);
}

/* FXDIV: w / a, rounded up at a half */
static os_error *fxdiv(bf w, bf a, bf *out)
{
    if (a.m == 0)
        return fm_err(FE_DIVBY0, NULL, NULL);
    if (w.m == 0) {
        *out = ZERO;
        return NULL;
    }
    bf r;
    r.s = a.s ^ w.s;
    r.e = w.e - a.e + 0x81;
    if (a.m == 0x80000000u) {
        r.m = w.m;
    } else {
        uint32_t q = 0, rem = w.m;
        int c = 0;
        for (int i = 0; i < 32; i++) {
            int bit = c || rem >= a.m;
            if (bit)
                rem -= a.m;
            q = q << 1 | (uint32_t)bit;
            c = (int)(rem >> 31);
            rem <<= 1;
        }
        if (!(q & 0x80000000u)) {                  /* one more bit, normalising */
            int bit = c || rem >= a.m;
            if (bit)
                rem -= a.m;
            q = q << 1 | (uint32_t)bit;
            c = (int)(rem >> 31);
            rem <<= 1;
            r.e--;
        }
        if (c || rem >= a.m) {
            if (++q == 0) {
                q = 0x80000000u;
                r.e++;
            }
        }
        r.m = q;
    }
    if ((r.e & ~255) == 0) {
        *out = r;
        return NULL;
    }
    if (r.e < 0) {
        *out = ZERO;
        return NULL;
    }
    return fm_err(FE_OVERFLOW, NULL, NULL);
}

/* FADDW: a + w, translated step by step from the original.  Its special
 * cases decide which operand's exponent and sign survive. */
static os_error *faddw(bf a, bf w, bf *out)
{
    uint32_t g = 0, m;
    int32_t d, sh;
    if (a.m == 0) {                                 /* FWTOA */
        *out = w;
        return NULL;
    }
    d = a.e - w.e;
    if (d == 0) {                                   /* 10 */
        g = 0;
        if (a.s == w.s) {
            uint64_t t = (uint64_t)w.m + a.m;
            m = (uint32_t)t;
            if (!(t >> 32))
                goto round;
            goto renorm_right;
        }
        m = a.m - w.m;
        if (a.m < w.m) {
            a.s = w.s;
            m = 0u - m;
        }
        if (m & 0x80000000u)
            goto done;
        goto renorm_left;
    }
    if (d < 0) {                                    /* a's exponent the smaller */
        sh = -d;
        a.e = w.e;
        if (sh > 32) {                              /* FSWTOA */
            a.s = w.s;
            a.m = w.m;
            *out = a;
            return NULL;
        }
        uint32_t lo = sh == 32 ? a.m : a.m << (32 - sh), hi = sh == 32 ? 0 : a.m >> sh;
        if (a.s == w.s) {
            g = lo;
            uint64_t t = (uint64_t)w.m + hi;
            m = (uint32_t)t;
            if (!(t >> 32))
                goto round;
            goto renorm_right;
        }
        g = 0u - lo;                                /* 04 */
        m = w.m - hi - (lo != 0);
        a.s = w.s;
        if (m & 0x80000000u)
            goto done;                              /* no rounding */
        goto renorm_left;
    }
    sh = d;                                         /* 06 */
    if (sh > 32) {
        *out = a;
        return NULL;
    }
    {
        uint32_t lo = sh == 32 ? w.m : w.m << (32 - sh), hi = sh == 32 ? 0 : w.m >> sh;
        if (a.s == w.s) {
            g = lo;
            uint64_t t = (uint64_t)a.m + hi;
            m = (uint32_t)t;
            if (!(t >> 32))
                goto round;
            goto renorm_right;
        }
        g = 0u - lo;                                /* 08 */
        m = a.m - hi - (lo != 0);
        if (m & 0x80000000u)
            goto done;
        goto renorm_left;
    }
renorm_right: {
        uint32_t b0 = m & 1;
        m = m >> 1 | 0x80000000u;
        g = g >> 1 | b0 << 31;
        if (++a.e >= 256)
            return fm_err(FE_OVERFLOW, NULL, NULL);
        goto round;
    }
renorm_left:                                        /* 12 */
    if (m == 0) {                                   /* FNRMB */
        m = g;
        if (m == 0) {
            *out = ZERO;
            return NULL;
        }
        a.e -= 32;
        if (a.e < 0) {
            *out = ZERO;
            return NULL;
        }
        int n = clz32(m);
        a.m = m << n;
        a.e -= n;
        *out = a;
        return NULL;
    }
    {
        int n = 0;
        do {
            m = m << 1 | g >> 31;
            g <<= 1;
            n++;
        } while (!(m & 0x80000000u));
        a.e -= n;
        if (a.e < 0) {                              /* incipient underflow */
            if (g < 0x80000000u) {
                *out = ZERO;
                return NULL;
            }
            if (g == 0x80000000u)
                m &= ~1u;
            if (++m == 0) {
                m = 0x80000000u;
                a.e++;
            }
            if (a.e < 0) {
                *out = ZERO;
                return NULL;
            }
            goto done;
        }
    }
round:                                              /* 16 */
    if (g >= 0x80000000u) {
        if (g == 0x80000000u)
            m &= ~1u;
        if (++m == 0) {
            m = 0x80000000u;
            if (++a.e >= 256)
                return fm_err(FE_OVERFLOW, NULL, NULL);
        }
    }
done:
    a.m = m;
    *out = a;
    return NULL;
}

/* ---- matrices -------------------------------------------------------------------------------------- */

/* matrix_float: the first four are 16.16, the last two integers */
void fm_matrix_float(const int32_t in[6], struct fm_fpmat *out)
{
    for (int i = 0; i < 6; i++) {
        bf f = iflt(in[i]);
        if (i < 4) {
            f.e -= 16;
            if (f.e < 0)
                f.e = 0;
        }
        out->v[i] = pack(f);
    }
}

/* A row of a times a column of b: a0*b0 + a1*b1 (rowtimescol_0) */
static os_error *rowcol(struct fm_fp a0, struct fm_fp a1, struct fm_fp b0, struct fm_fp b1, bf *out)
{
    bf p = unpack(a0), q = unpack(a1);
    os_error *e = NULL;
    if (p.m)
        e = fmul(p, unpack(b0), &p);
    if (!e && q.m)
        e = fmul(q, unpack(b1), &q);
    if (!e)
        e = faddw(q, p, out);
    return e;
}

/* matrix_multiply: out = a then b.  If either is NULL, out is the other. */
os_error *fm_matrix_mul(const struct fm_fpmat *a, const struct fm_fpmat *b, struct fm_fpmat *out)
{
    if (!a || !b) {
        *out = a ? *a : *b;
        return NULL;
    }
    struct fm_fpmat r;
    bf v;
    os_error *e;
    static const int col0[2] = { 0, 1 };           /* the columns of b: XX,XY and YX,YY */
    for (int row = 0; row < 3; row++)
        for (int c = 0; c < 2; c++) {
            struct fm_fp b0 = b->v[col0[c]], b1 = b->v[col0[c] + 2];
            if ((e = rowcol(a->v[2 * row], a->v[2 * row + 1], b0, b1, &v)))
                return e;
            if (row == 2 && (e = faddw(v, unpack(b->v[4 + c]), &v)))    /* the implicit 1 */
                return e;
            r.v[2 * row + c] = pack(v);
        }
    *out = r;
    return NULL;
}

/* matrix_fix */
os_error *fm_matrix_fix(const struct fm_fpmat *in, struct fm_mat *out)
{
    int32_t cs = 0;
    for (int i = 0; i < 4; i++) {
        int32_t t = (int32_t)(in->v[i].w & 255) - (0x80 + 15);
        if (t > cs)
            cs = t;
    }
    struct fm_mat r;
    r.cs = cs;
    for (int i = 0; i < 6; i++) {
        bf f = { in->v[i].m, (int32_t)(in->v[i].w & 255), in->v[i].w & 0x80000000u };
        if (i < 4)
            f.e += 16 - cs;
        os_error *e = intrnd(f, &r.v[i]);
        if (e)
            return e;
    }
    *out = r;
    return NULL;
}

/* matrix_double: double the first four by adding 1 to their exponent
 * words */
void fm_matrix_double(struct fm_fpmat *m)
{
    for (int i = 0; i < 4; i++)
        m->v[i].w += 1;
}

/* [x 0 0 y 0 0] (makematrix) */
static struct fm_fpmat diagonal(struct fm_fp x, struct fm_fp y)
{
    struct fm_fpmat m;
    memset(&m, 0, sizeof m);
    m.v[0] = x;
    m.v[3] = y;
    return m;
}

/* getxysizeover16 */
struct fm_fpmat fm_size_matrix(int32_t xsize, int32_t ysize)
{
    bf x = iflt(xsize), y = iflt(ysize);
    x.e -= 4;
    y.e -= 4;
    return diagonal((struct fm_fp){ x.m, (uint32_t)x.e }, (struct fm_fp){ y.m, (uint32_t)y.e });
}

/* getdesignmatrix: a diagonal matrix with 1000/designsize on both axes */
os_error *fm_design_matrix(int32_t designsize, struct fm_fpmat *out)
{
    bf d;
    os_error *e = fxdiv((bf){ 1000u << 22, 0x80 + 32 - 22, 0 }, iflt(designsize), &d);
    if (!e)
        *out = diagonal((struct fm_fp){ d.m, (uint32_t)d.e }, (struct fm_fp){ d.m, (uint32_t)d.e });
    return e;
}

/* div72000: (v << 9) / 72000, which is how the resolution matrix holds it */
os_error *fm_res_value(int32_t v, struct fm_fp *out)
{
    bf a = iflt(v);
    os_error *e = NULL;
    if (a.m) {
        a.e += 9;
        e = fxdiv(a, iflt(72000), &a);
    }
    if (!e)
        *out = (struct fm_fp){ a.m, (uint32_t)a.e };
    return e;
}

struct fm_fpmat fm_res_matrix(struct fm_fp xx, struct fm_fp yy)
{
    return diagonal(xx, yy);
}

/* ---- transforming ------------------------------------------------------------------------------ */

void fm_transform_pt(const struct fm_mat *m, int32_t *x, int32_t *y)
{
    int32_t xs = (int32_t)((uint32_t)*x << m->cs), ys = (int32_t)((uint32_t)*y << m->cs);
    int64_t px = (int64_t)xs * m->v[0] + (int64_t)ys * m->v[2];
    int64_t py = (int64_t)xs * m->v[1] + (int64_t)ys * m->v[3];
    *x = (int32_t)((uint32_t)(uint64_t)(px >> 16) + (uint32_t)m->v[4]);
    *y = (int32_t)((uint32_t)(uint64_t)(py >> 16) + (uint32_t)m->v[5]);
}

/* transformbox: the bounding box of the transformed box.  b is x0, y0, x1,
 * y1. */
void fm_transform_box(const struct fm_mat *m, int32_t b[4])
{
    if (!(m->v[1] | m->v[2]) && (m->v[0] | m->v[3]) >= 0) {
        int32_t x1 = b[2], y1 = b[3];
        fm_transform_pt(m, &x1, &y1);
        fm_transform_pt(m, &b[0], &b[1]);
        b[2] = x1, b[3] = y1;
        return;
    }
    int32_t px[4] = { b[0], b[0], b[2], b[2] }, py[4] = { b[1], b[3], b[1], b[3] };
    for (int i = 0; i < 4; i++)
        fm_transform_pt(m, &px[i], &py[i]);
    /* GetCC: compare the first two, then compare the others with the result */
    int32_t v;
    v = px[0] < px[1] ? px[0] : px[1];
    if (px[2] < v) v = px[2];
    if (px[3] < v) v = px[3];
    b[0] = v;
    v = py[0] < py[1] ? py[0] : py[1];
    if (py[2] < v) v = py[2];
    if (py[3] < v) v = py[3];
    b[1] = v;
    v = px[0] > px[1] ? px[0] : px[1];
    if (px[2] > v) v = px[2];
    if (px[3] > v) v = px[3];
    b[2] = v;
    v = py[0] > py[1] ? py[0] : py[1];
    if (py[2] > v) v = py[2];
    if (py[3] > v) v = py[3];
    b[3] = v;
}

/* squareroot: floor(sqrt(v)), done bit by bit as the original does */
static uint32_t square_root(uint32_t v)
{
    if (v == 0)
        return 0;
    int32_t n = 0;
    for (uint32_t t = v; t; t >>= 1)
        n++;
    n = (n - 1) >> 1;
    uint32_t r2 = 1u << n, r5 = r2, r3 = r2 << n, r4 = r3;
    while (r3 < v) {
        uint32_t r6 = r3 + (r2 << n);
        r4 >>= 2;
        r5 >>= 1;
        r6 += r4;
        if (r6 <= v)
            r3 = r6, r2 += r5;
        if (n-- <= 1)
            break;
    }
    return r2;
}

/* getpixsize: 1 em in 1/16 pixel.  *big is set if the result is too big to
 * multiply up. */
static int32_t pix_size(int32_t d, int32_t v, int *big)
{
    int32_t r = (int32_t)(uint32_t)(uint64_t)(((int64_t)v * d) >> 21);
    int32_t a = r < 0 ? (int32_t)(0u - (uint32_t)r) : r;
    *big = a > 0x8000;
    return r;
}

/* transformxyscale: a transformed font's width and height, for comparing
 * with the thresholds (in pixels * 72 * 16).  *swap is set if the font is
 * more vertical than horizontal. */
void fm_transform_xyscale(const struct fm_mat *render, int32_t designsize, int32_t *xscale,
                          int32_t *yscale, uint8_t *swap)
{
    int32_t d = (int32_t)((uint32_t)designsize << render->cs);
    int big;
    int32_t xx = pix_size(d, render->v[0], &big), yx = 0, xy = 0, yy = 0;
    uint32_t l = 0;
    if (!big) {
        yx = pix_size(d, render->v[1], &big);
    }
    if (!big) {
        int32_t axx = xx < 0 ? -xx : xx, ayx = yx < 0 ? -yx : yx;
        *swap = axx < ayx ? 1 : 0;
        l = square_root((uint32_t)yx * (uint32_t)yx + (uint32_t)xx * (uint32_t)xx);
        big = l == 0;
    }
    if (!big)
        xy = pix_size(d, render->v[2], &big);
    if (!big)
        yy = pix_size(d, render->v[3], &big);
    if (!big) {
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int32_t sx = (int32_t)((uint32_t)xx + (uint32_t)xy), sy = (int32_t)((uint32_t)yx + (uint32_t)yy);
        if (x0 > xx) x0 = xx;
        if (x0 > xy) x0 = xy;
        if (x0 > sx) x0 = sx;
        if (y0 > yx) y0 = yx;
        if (y0 > yy) y0 = yy;
        if (y0 > sy) y0 = sy;
        if (x1 < xx) x1 = xx;
        if (x1 < xy) x1 = xy;
        if (x1 < sx) x1 = sx;
        if (y1 < yx) y1 = yx;
        if (y1 < yy) y1 = yy;
        if (y1 < sy) y1 = sy;
        if ((int32_t)((uint32_t)x1 - (uint32_t)x0) > 364 * 16 ||
            (int32_t)((uint32_t)y1 - (uint32_t)y0) > 364 * 16)
            big = 1;
    }
    if (!big) {
        int32_t a = (int32_t)((uint32_t)xx * (uint32_t)yy - (uint32_t)yx * (uint32_t)xy);
        uint32_t h = (a < 0 ? 0u - (uint32_t)a : (uint32_t)a) / l;
        if ((int32_t)h > 0x1C00000)
            big = 1;
        else {
            *xscale = (int32_t)(l * 72u);
            *yscale = (int32_t)(h * 72u);
            return;
        }
    }
    *xscale = *yscale = 0x20000000;
}
