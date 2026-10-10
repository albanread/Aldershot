/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2001 Pace Micro Technology plc
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
 * (Sources/Video/Render/Super: s.Super, c.Matrix1, c.Matrix2).
 */

/* supersample.c: SuperSample (Video/Render/Super), reimplemented. It turns
 * 1-bpp images into 4-bpp images, reduced by 4 in each direction, for the
 * Font Manager's anti-aliased characters.
 *
 * Super_Sample90 weights a 7x7 grid (1 2 3 4 3 2 1 each way). Sample45
 * weights a 9x7 grid, for half-height pixels. Each row of the grid is looked
 * up whole in a table (Matrix1 or Matrix2), as the original does. The code
 * is s/Super, transliterated, at version 16, so the light-pixel threshold
 * applies. Each output word is eight pixels. The eighth pixel takes bits
 * from the next input word. There Sample45 is one column off. It is
 * centred on bit 31 of the word, and not on bit 0 of the next word. A row's
 * last pixel is left 0. The output is written as it is made, so it may
 * overwrite the input.
 */
#include <stdint.h>

#include "rosgd/api.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"

struct ros_module supersample_module;

static uint8_t table90[128], m1[512], m3[512];

static void tables(void)
{
    static const int w90[7] = { 1, 2, 3, 4, 3, 2, 1 };
    static const int w1[9] = { 1, 2, 4, 6, 6, 6, 4, 2, 1 };
    static const int w3[9] = { 1, 4, 8, 12, 13, 12, 8, 4, 1 };
    for (int x = 0; x < 512; x++) {
        int a = 0, b = 0, c = 0;
        for (int bit = 0; bit < 9; bit++)
            if (x >> bit & 1) {
                if (bit < 7)
                    a += w90[bit];
                b += w1[bit], c += w3[bit];
            }
        if (x < 128)
            table90[x] = (uint8_t)a;
        m1[x] = (uint8_t)b, m3[x] = (uint8_t)c;
    }
}

/* Turn total, which already includes the rounding of 14, into a nibble at
 * the top of a word. A nibble of 16 or more (bit 8 of total, carried out)
 * becomes 15, and a nibble of 1 or less becomes 0. */
static uint32_t nibble(uint32_t total)
{
    uint32_t v = (total & 0x100) ? 0xF0000000u : (total << 24) & 0xF0000000u;
    return v <= 0x10000000u ? 0 : v;
}

static os_error *check(uint32_t gap, uint32_t rows)
{
    if ((rows & 0x80000003u) != 3 || (gap & 0x80000003u))
        return ros_error(0x1EA, "Bad parameters");
    return NULL;
}

/* This returns R0. It is the last pixel made (the "total" register), or 3
 * if there are no rows. */
static uint32_t sample(uint32_t in, uint32_t gap, uint32_t rows, uint32_t out, int s45)
{
    int32_t rowcount = (int32_t)(rows - 3);
    uint32_t last = 0;
    if (rowcount == 0)
        return 3;
    uint32_t colcount = gap, inptr = in;
    do {
        uint32_t w[7];
        for (int k = 0; k < 7; k++)
            w[k] = ros_ld32(inptr + (uint32_t)k * gap);
        uint32_t outword;
        for (;;) {
            uint32_t shift = 0;
            outword = 0x08000000u;
            for (;;) {
                uint32_t total;
                if (!s45) {
                    static const uint32_t mul[7] = { 1, 2, 3, 4, 3, 2, 1 };
                    total = 0;
                    for (int k = 0; k < 7; k++)
                        total += mul[k] * table90[(w[k] >> shift) & 0x7F];
                    total += 14;
                } else {
                    uint32_t x = (w[1] >> shift) & 0x1FF;
                    total = x ? m1[x] : 0;
                    if ((w[0] >> shift) & 0x10)
                        total++;
                    x = (w[2] >> shift) & 0x1FF;
                    total += m3[x];
                    x = (w[3] >> shift) & 0x1FF;
                    if (x)
                        total += m3[x] + ((x >> 4) & 1);
                    total += m3[(w[4] >> shift) & 0x1FF];
                    total += m1[(w[5] >> shift) & 0x1FF];
                    total += 14 + ((((w[6] >> shift) & 0x1FF) >> 4) & 1);
                }
                uint32_t c = (outword >> 3) & 1;
                last = nibble(total);
                outword = last | outword >> 4;
                if (c)
                    break;
                shift += 4;
            }
            colcount -= 4;
            if (colcount == 0)
                break;
            /* the eighth pixel, with the next words */
            inptr += 4;
            uint32_t nw[7], total;
            for (int k = 0; k < 7; k++)
                nw[k] = ros_ld32(inptr + (uint32_t)k * gap);
            if (!s45) {
                static const uint32_t mul[7] = { 1, 2, 3, 4, 3, 2, 1 };
                total = 14;
                for (int k = 0; k < 7; k++)
                    total += mul[k] * table90[(w[k] >> 3 | nw[k] << 29) >> 25];
            } else {
                total = w[0] >> 31;
                total += m1[(w[1] >> 4 | nw[1] << 28) >> 23];
                total += m3[(w[2] >> 4 | nw[2] << 28) >> 23];
                uint32_t r = w[3] >> 4 | nw[3] << 28;
                total += m3[r >> 23] + ((r >> 27) & 1);
                total += m3[(w[4] >> 4 | nw[4] << 28) >> 23];
                total += m1[(w[5] >> 4 | nw[5] << 28) >> 23];
                total += (w[6] >> 31) + 14;
            }
            last = nibble(total);
            outword = last | outword >> 4;
            ros_st32(out, outword);
            out += 4;
            for (int k = 0; k < 7; k++)
                w[k] = nw[k];
        }
        ros_st32(out, outword >> 4);
        out += 4;
        colcount = gap;
        inptr += 4 + 3 * gap;
        rowcount -= 4;
    } while (rowcount > 0);
    return last;
}

static void swi(struct ros_cpu *s, int s45)
{
    os_error *e = check(s->r[2], s->r[3]);
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    s->r[0] = sample(s->r[1], s->r[2], s->r[3], s->r[4], s45);
    s->v = 0;
}

void ros_thunk_Super_Sample90(struct ros_cpu *s) { swi(s, 0); }
void ros_thunk_Super_Sample45(struct ros_cpu *s) { swi(s, 1); }

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    tables();
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6, "SWI value out of range for module SuperSample");
}

struct ros_module supersample_module = {
    .title = "SuperSample",
    .help = "SuperSample\t0.16 (14 Jan 2012) ROSGD native",
    .init = init,
    .bad_swi = bad_swi,
    .swi_chunk = 0x40D80,
    .swi_thunks = ros_swi_thunks_Super,
    .swi_names = ros_swi_names_Super,
    .swi_prefix = "Super",
};

__attribute__((constructor)) static void count(void)
{
    supersample_module.swi_count = ros_swi_count_Super;
}
