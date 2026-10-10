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
 * (Sources/Video/Render/Colours: s.FontColour).
 */

/* fontcol.c: ColourTrans's font colours (s/FontColour).
 *
 * At 8 bpp and more, the Font Manager is told the colours themselves
 * (Font_SetPalette with "True"). Below that, the colours are chosen for
 * anti-aliasing. The code searches the palette for a run of up to 14
 * entries, going up or down from the foreground. Each entry must be near
 * enough to the step that it stands for. If no such run exists, it tries
 * fewer entries, down to none.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "ct.h"

#define XFont_CurrentFont 0x6008Bu
#define XFont_SetFontColours 0x60092u
#define XFont_SetPalette 0x60093u
#define XFont_SetColourTable 0x600A2u

static os_error *font_swi(uint32_t n, uint32_t r[10])
{
    os_error *e;
    ct_swi(n, r, &e);
    return e;
}

/* Returns the colour number of a colour. It caches the result in the way
 * that ReturnFontColours does. */
static uint32_t number(struct ct_pal *p, int *built, uint32_t c)
{
    struct ct_cache *e = ct_lookup(c);
    if (e)
        return e->colour;
    if (!*built)
        ct_build(0xFFFFFFFFu, 0xFFFFFFFFu, p), *built = 1;
    uint32_t n = ct_best(p, c);
    ct_write_cache(n, n, c);
    return n;
}

/* advanceRGB's test. It compares the palette entry with the stepped colour. */
static int near(const uint32_t cur[3], uint32_t w, uint32_t E)
{
    uint32_t L = ct.loading, err = 0;
    for (int k = 0; k < 3; k++) {
        int32_t d = (int32_t)(cur[k] >> 8) - (int32_t)((w >> (8 * (k + 1))) & 255);
        err += (uint32_t)(d * d) * ((L >> (8 * (k + 1))) & 255);
    }
    return (int32_t)err < (int32_t)E;
}

static int run(const struct ct_pal *p, uint32_t fg, uint32_t fgc, uint32_t step[3], uint32_t down,
               int32_t N, uint32_t E, int dir)
{
    uint32_t cur[3] = { ((fg >> 8) & 255) << 8, ((fg >> 16) & 255) << 8, (fg >> 24) << 8 };
    int32_t i = (int32_t)fgc;
    uint32_t k = (uint32_t)N;
    do {
        i += dir;
        if (i < 0 || i >= (int32_t)p->n)
            return 0;
        for (int c = 0; c < 3; c++)
            cur[c] = (down & (1u << c) ? cur[c] - step[c] : cur[c] + step[c]) & 0xFFFF;
        if (!near(cur, p->t[i], E))
            return 0;
    } while (--k != 0);
    return 1;
}

/* returnfontcolours_slow. R1 to R3 are as for Font_SetFontColours. */
static void slow(struct ros_cpu *s)
{
    struct ct_pal p;
    int built = 0;
    uint32_t bg = s->r[1] & ~0xFFu, fg = s->r[2] & ~0xFFu;
    uint32_t bgc = number(&p, &built, bg) & 255, fgc = number(&p, &built, fg) & 255;
    if (!built)
        ct_build(0xFFFFFFFFu, 0xFFFFFFFFu, &p);
    int32_t N = (int32_t)s->r[3];
    if (N > 14)
        N = 14;
    uint32_t diff[3], down = 0;
    for (int c = 0; c < 3; c++) {
        int32_t d = (int32_t)(((bg >> (8 * (c + 1))) & 255) << 8) -
                    (int32_t)(((fg >> (8 * (c + 1))) & 255) << 8);
        if (d < 0)
            d = -d, down |= 1u << c;
        diff[c] = (uint32_t)d;
    }
    s->r[1] = bgc;
    for (;; N--) {
        if (N < 0) {
            s->r[2] = fgc, s->r[3] = 0;
            return;
        }
        uint32_t d = (uint32_t)N + 1, step[3], E = 0, L = ct.loading;
        for (int c = 0; c < 3; c++) {
            step[c] = diff[c] / d;
            uint32_t w = step[c] >> 8;
            E += w * w * ((L >> (8 * (c + 1))) & 255);
        }
        if (run(&p, fg, fgc, step, down, N, E, 1)) {
            s->r[2] = fgc + (uint32_t)N, s->r[3] = (uint32_t)-N;
            return;
        }
        if (run(&p, fg, fgc, step, down, N, E, -1)) {
            s->r[2] = fgc - (uint32_t)N, s->r[3] = (uint32_t)N;
            return;
        }
    }
}

/* ReturnFontColours_Alt. If set is true it sets the colours, as
 * SetFontColours does. Otherwise it leaves the Font Manager's colours as
 * they were. */
static os_error *alt(struct ros_cpu *s, int set)
{
    if ((int32_t)s->r[0] <= -1)
        s->r[0] = 0;
    if (ct.c_l2bpp >= 3) {
        uint32_t bg = s->r[1], fg = s->r[2], off = s->r[3];
        os_error *e;
        if (set) {
            uint32_t r[10] = { s->r[0], bg, fg & 15, off, bg, fg, 0x65757254 };
            e = font_swi(XFont_SetPalette, r);
        } else {
            uint32_t c[10] = { 0 };
            font_swi(XFont_CurrentFont, c);
            uint32_t r[10] = { c[0], bg, fg & 15, off, bg, fg, 0x65757254 };
            e = font_swi(XFont_SetPalette, r);
            uint32_t back[10] = { 0, c[1], c[2], c[3] };
            font_swi(XFont_SetFontColours, back);
        }
        s->r[2] = fg & 15;
        return e;
    }
    slow(s);
    if (set) {
        uint32_t r[10] = { s->r[0], s->r[1], s->r[2], s->r[3] };
        return font_swi(XFont_SetFontColours, r);
    }
    return NULL;
}

os_error *ct_font_colours(struct ros_cpu *s, int set)
{
    if (!set)
        return alt(s, 0);
    uint32_t in[4] = { s->r[0], s->r[1], s->r[2], s->r[3] };
    struct ros_cpu t = *s;
    os_error *e = alt(&t, 1);
    uint32_t out[4] = { in[0], in[1], in[2], in[3] };
    if (!e) {
        out[1] = t.r[1], out[2] = t.r[2], out[3] = t.r[3];
        uint32_t r[10] = { t.r[0], 0, in[1], in[2], in[3] };
        e = font_swi(XFont_SetColourTable, r);
    }
    if (e) {
        uint32_t r[10] = { out[0], out[1], out[2], out[3] };
        e = font_swi(XFont_SetFontColours, r);
    }
    s->r[1] = out[1], s->r[2] = out[2], s->r[3] = out[3];
    return e;
}
