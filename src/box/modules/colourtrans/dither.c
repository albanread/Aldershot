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
 * (Sources/Video/Render/Colours: s.Dither).
 */

/* dither.c: ColourTrans's dither patterns (s/Dither).
 *
 * A colour that the palette lacks is drawn as a pattern for OS_SetColour.
 * At 1 bpp the pattern is a 4 x 4 ordered dither of the colour's brightness.
 * At 2, 4 and 8 bpp the code finds four colours by passing the error on from
 * one to the next. At 16 bpp it does the same but truncates the colours
 * instead of reading a palette. It sorts the four colours and pairs them
 * into a 2 x 2 cell, so that the two most alike sit on a diagonal. The cell
 * is tiled over the pattern's words.
 */
#include <string.h>

#include "rosgd/api.h"
#include "ct.h"

static int32_t clamp(int32_t x)
{
    return x < 0 ? 0 : x > 255 ? 255 : x;
}

/* getpatternmono */
static void mono(const struct ct_pal *p, uint32_t c, uint32_t pat[4])
{
    static const uint8_t thr[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 },
                                       { 15, 7, 13, 5 } };
    uint32_t bg = ct_best(p, 0) & 1 ? 0xFFFFFFFFu : 0;
    uint32_t X = 77 * ((c >> 8) & 255) + 150 * ((c >> 16) & 255) + 28 * (c >> 24);
    uint32_t I = (((X + 0x7F) * 0x101) + 0x100) >> 16;
    I = (I * 0x10F5C) >> 20;
    for (int y = 0; y < 4; y++) {
        uint32_t v = bg;
        for (int x = 0; x < 4; x++)
            if ((int32_t)I > thr[y][x])
                v ^= 1u << x;
        v &= 15, v |= v << 4, v |= v << 8, v |= v << 16;
        pat[y] = v;
    }
}

/* The shared tail. v holds the four colours as &00BBGGRR and n holds their
 * colour numbers. */
static void cell(uint32_t v[4], uint32_t n[4], uint32_t bpp, uint32_t pat[4])
{
    static const int pairs[6][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 } };
    for (int k = 0; k < 6; k++) {
        int i = pairs[k][0], j = pairs[k][1];
        if ((int32_t)v[i] <= (int32_t)v[j]) {
            uint32_t t = v[i]; v[i] = v[j]; v[j] = t;
            t = n[i]; n[i] = n[j]; n[j] = t;
        }
    }
    uint32_t L = ct.loading, e[4];
    for (int k = 0; k < 4; k++)
        e[k] = ((L >> 8) & 255) * (v[k] & 255) + ((L >> 16) & 255) * ((v[k] >> 8) & 255) +
               (L >> 24) * ((v[k] >> 16) & 255);
#define D(a, b) ((int32_t)(e[a] - e[b]) < 0 ? (int32_t)(e[b] - e[a]) : (int32_t)(e[a] - e[b]))
    int32_t best = D(0, 1);
    uint32_t r0 = n[0] | n[2] << bpp, r1 = n[3] | n[1] << bpp;
    if (e[0] != e[1]) {
        if (D(0, 2) < best) best = D(0, 2), r0 = n[0] | n[1] << bpp, r1 = n[3] | n[2] << bpp;
        if (D(0, 3) < best) best = D(0, 3), r0 = n[0] | n[1] << bpp, r1 = n[2] | n[3] << bpp;
        if (D(1, 2) < best) best = D(1, 2), r0 = n[1] | n[0] << bpp, r1 = n[3] | n[2] << bpp;
        if (D(1, 3) < best) best = D(1, 3), r0 = n[1] | n[0] << bpp, r1 = n[2] | n[3] << bpp;
        if (D(2, 3) < best) best = D(2, 3), r0 = n[2] | n[0] << bpp, r1 = n[1] | n[3] << bpp;
    }
#undef D
    for (uint32_t s = bpp; (s = (s << 1) & 31) != 0;)
        r0 |= r0 << s, r1 |= r1 << s;
    pat[0] = r0, pat[1] = r1, pat[2] = r0, pat[3] = r1;
}

static uint32_t rgb(int32_t r, int32_t g, int32_t b)
{
    return (uint32_t)clamp(b) << 24 | (uint32_t)clamp(g) << 16 | (uint32_t)clamp(r) << 8;
}

#define R_(x) (((x) >> 8) & 255)
#define G_(x) (((x) >> 16) & 255)
#define B_(x) ((x) >> 24)

/* getpatterncolour: 2, 4 and 8 bpp, reading colours through the palette. */
static void colour(const struct ct_pal *p, uint32_t c, uint32_t bpp, uint32_t pat[4])
{
    int32_t r2 = 2 * (int32_t)R_(c), g2 = 2 * (int32_t)G_(c), b2 = 2 * (int32_t)B_(c);
    uint32_t n[4], P[4];
    n[0] = ct_best(p, c), P[0] = (p->t[n[0] & 255] & ~0xFFu) | (n[0] & 255);
    uint32_t t = rgb(r2 - (int32_t)R_(P[0]), g2 - (int32_t)G_(P[0]), b2 - (int32_t)B_(P[0]));
    n[1] = ct_best(p, t), P[1] = (p->t[n[1] & 255] & ~0xFFu) | (n[1] & 255);
    t = rgb(r2 - (int32_t)((R_(P[0]) + R_(P[1])) >> 1), g2 - (int32_t)((G_(P[0]) + G_(P[1])) >> 1),
            b2 - (int32_t)((B_(P[0]) + B_(P[1])) >> 1));
    n[2] = ct_best(p, t), P[2] = (p->t[n[2] & 255] & ~0xFFu) | (n[2] & 255);
    t = rgb(r2 - (int32_t)((R_(P[0]) + R_(P[1]) + R_(P[2])) / 3),
            g2 - (int32_t)((G_(P[0]) + G_(P[1]) + G_(P[2])) / 3),
            b2 - (int32_t)((B_(P[0]) + B_(P[1]) + B_(P[2])) / 3));
    n[3] = ct_best(p, t), P[3] = p->t[n[3] & 255] & ~0xFFu;
    uint32_t v[4], num[4] = { P[0] & 255, P[1] & 255, P[2] & 255, n[3] };
    for (int k = 0; k < 4; k++)
        v[k] = P[k] >> 8;
    cell(v, num, bpp, pat);
}

/* getpatterncolour16: the colours are truncated. They are not read from a
 * palette. */
static void colour16(const struct ct_pal *p, uint32_t c, uint32_t pat[4])
{
    uint32_t M = ct.c_l2nc == 12 ? 0xF0F0F0 : ct.c_flags & MF_64K ? 0xF8FCF8 : 0xF8F8F8;
    int32_t r2 = 2 * (int32_t)R_(c), g2 = 2 * (int32_t)G_(c), b2 = 2 * (int32_t)B_(c);
    uint32_t v[4], n[4];
    v[0] = M & (c >> 8), n[0] = ct_best(p, c);
    uint32_t t = rgb(r2 - (int32_t)(v[0] & 255), g2 - (int32_t)((v[0] >> 8) & 255),
                     b2 - (int32_t)(v[0] >> 16)) >> 8;
    v[1] = M & t, n[1] = ct_best(p, t << 8);
    t = rgb(r2 - (int32_t)(((v[0] & 255) + (v[1] & 255)) >> 1),
            g2 - (int32_t)((((v[0] >> 8) & 255) + ((v[1] >> 8) & 255)) >> 1),
            b2 - (int32_t)(((v[0] >> 16) + (v[1] >> 16)) >> 1)) >> 8;
    v[2] = M & t, n[2] = ct_best(p, t << 8);
    t = rgb(r2 - (int32_t)(((v[0] & 255) + (v[1] & 255) + (v[2] & 255)) / 3),
            g2 - (int32_t)((((v[0] >> 8) & 255) + ((v[1] >> 8) & 255) + ((v[2] >> 8) & 255)) / 3),
            b2 - (int32_t)(((v[0] >> 16) + (v[1] >> 16) + (v[2] >> 16)) / 3)) >> 8;
    v[3] = M & t, n[3] = ct_best(p, t << 8);
    cell(v, n, 16, pat);
}

/* Makes the pattern for the colour that a cache entry holds. It chooses the
 * routine as SetGCOL does, where 16 bpp has its own routine. It also serves
 * MiscOp 1. In 16 and 32 bpp the original sends MiscOp 1 through the palette
 * routine with no palette. Here it uses the 16 bpp routine instead. */
void ct_pattern(struct ct_cache *e, uint32_t d, int for_setgcol)
{
    (void)for_setgcol;
    struct ct_pal p;
    if (ct_build(0xFFFFFFFFu, 0xFFFFFFFFu, &p))
        return;
    uint32_t c = e->key << 8;
    if (d == 0)
        mono(&p, c, e->pat);
    else if (d >= 4)
        colour16(&p, c, e->pat);
    else
        colour(&p, c, 1u << d, e->pat);
}
