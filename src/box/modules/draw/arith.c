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
 * (Sources/Video/Render/DrawMod: s.DrArith) and of the kernel's
 * (Sources/Kernel: s.HeapSort).
 */

/* arith.c: the double precision routines of s/DrArith, and the kernel's
 * heapsort.
 *
 * The DrArith routines are transliterated bit for bit. Their results for
 * arguments outside their stated ranges are the original's too. The heapsort
 * is Kernel/s/HeapSort, which Draw sorts its edges with. It is not a stable
 * sort, so its order of equal items is kept.
 */
#include "draw.h"

/* SSmultD: a signed 32-bit multiply with a 64-bit result (SMULL). */
void draw_ssmultd(int32_t a, int32_t b, uint32_t *lo, uint32_t *hi)
{
    int64_t p = (int64_t)a * b;
    *lo = (uint32_t)p, *hi = (uint32_t)((uint64_t)p >> 32);
}

/* arith_DSmultD: a signed double times a signed single. The result is the
 * low 64 bits. */
void draw_dsmultd(uint32_t lo, uint32_t hi, int32_t s, uint32_t *rlo, uint32_t *rhi)
{
    uint64_t p = ((uint64_t)hi << 32 | lo) * (uint64_t)(int64_t)s;
    *rlo = (uint32_t)p, *rhi = (uint32_t)(p >> 32);
}

/* The restoring division loop that both DSdiv routines share. It takes 32
 * steps. A sentinel bit starts in the quotient, and the loop ends when that
 * bit is shifted out. The dividend's high word loses what it shifts out. */
static uint32_t dsdiv_loop(uint32_t lo, uint32_t hi, uint32_t d)
{
    uint32_t q = 1;
    for (;;) {
        uint32_t c = lo >> 31;
        lo <<= 1;
        hi = hi << 1 | c;
        uint32_t bit = hi >= d;
        if (bit)
            hi -= d;
        uint32_t out = q >> 31;
        q = q << 1 | bit;
        if (out)
            return q;
    }
}

/* arith_DSdivS: unsigned double divided by unsigned single, giving a single
 * quotient. The quotient must fit in 32 bits. */
uint32_t draw_dsdivs(uint32_t lo, uint32_t hi, uint32_t d)
{
    return dsdiv_loop(lo, hi, d);
}

/* arith_DSdivD: unsigned double divided by unsigned single, giving a double
 * quotient. */
void draw_dsdivd(uint32_t lo, uint32_t hi, uint32_t d, uint32_t *qlo, uint32_t *qhi)
{
    *qhi = hi / d;
    *qlo = dsdiv_loop(lo, hi % d, d);
}

/* arith_DDdivS: signed double divided by unsigned double, giving a single
 * quotient. The quotient is rounded towards minus infinity, by taking one's
 * complements of a negative dividend and of the quotient. */
int32_t draw_dddivs(uint32_t lo, uint32_t hi, uint32_t dlo, uint32_t dhi)
{
    int neg = (int32_t)hi < 0;
    if (neg)
        lo = ~lo, hi = ~hi;
    uint32_t r0 = lo, r1 = hi, r2 = 0, q = 1;
    for (;;) {
        uint32_t c0 = r0 >> 31, c1 = r1 >> 31;
        r0 <<= 1;
        r1 = r1 << 1 | c0;
        r2 = r2 << 1 | c1;
        uint64_t top = (uint64_t)r2 << 32 | r1, dv = (uint64_t)dhi << 32 | dlo;
        uint32_t bit = top >= dv;
        if (bit) {
            top -= dv;
            r1 = (uint32_t)top, r2 = (uint32_t)(top >> 32);
        }
        uint32_t out = q >> 31;
        q = q << 1 | bit;
        if (out)
            break;
    }
    return (int32_t)(neg ? ~q : q);
}

/* arith_DsqrtS: the square root of an unsigned double, found a bit at a
 * time. */
uint32_t draw_dsqrts(uint32_t r0, uint32_t r1)
{
    uint32_t r2 = 0, r3 = 0x80000000u, r4 = 0, r5 = 0;
    do {
        r4 += r2 >> 1, r5 += r3 >> 1;
        uint64_t a = (uint64_t)r1 << 32 | r0, b = (uint64_t)r5 << 32 | r4;
        if (a >= b) {
            a -= b;
            r0 = (uint32_t)a, r1 = (uint32_t)(a >> 32);
            r4 += r2 >> 1, r5 += r3 >> 1;
        } else {
            r4 -= r2 >> 1, r5 -= r3 >> 1;
        }
        uint32_t c = r5 & 1;
        r5 >>= 1;
        r4 = r4 >> 1 | c << 31;
        c = (r3 >> 1) & 1;
        r3 >>= 2;
        r2 = c ? 0x80000000u : r2 >> 2;
    } while (r2 | r3);
    return r4;
}

/* HeapSortRoutine32 (Knuth's algorithm H), over a[0..n-1]. less(x, y) is the
 * procedure's LT. */
void draw_heapsort(uint32_t n, uint32_t *a, int (*less)(uint32_t x, uint32_t y, void *ctx),
                   void *ctx)
{
    if (n < 2)
        return;
    uint32_t *r = a - 1;                        /* R(1..n) */
    uint32_t l = (n >> 1) + 1, rr = n, i, j, R, K;
    for (;;) {
        if (l != 1) {                           /* h2 */
            l--;
            R = K = r[l];
        } else {
            R = K = r[rr];
            r[rr] = r[1];
            rr--;
            if (rr == 1)
                r[1] = R;
        }
        if (rr == 1)
            return;
        j = l;                                  /* h3 */
        for (;;) {
            i = j;                              /* h4 */
            j <<= 1;
            if (j > rr)
                break;
            if (j != rr && less(r[j], r[j + 1], ctx))       /* h5 */
                j++;
            if (!less(K, r[j], ctx))            /* h6 */
                break;
            r[i] = r[j];
        }
        r[i] = R;                               /* h8 */
    }
}
