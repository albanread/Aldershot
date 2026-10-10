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
 * This file is a reimplementation in C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.vdu.vduplot, s.vdu.vdugrafa).
 */
/* plot.h -- what plot.c, shapes.c and vdu5.c share. That is the pixel and
 * span primitives, and the kernel's Bresenham line state (GenLineParm). */
#ifndef ROSGD_VDU_PLOT_H
#define ROSGD_VDU_PLOT_H

#include <stdint.h>

extern const uint32_t *plot_gcol;       /* GColAdr: eight {ora, eor} rows */

uint32_t plot_npix(void);
uint32_t *plot_word(int32_t x, int32_t y);
uint32_t plot_pmask(int32_t x);
uint32_t plot_erow(int32_t y);
int plot_in_window(int32_t x, int32_t y);
void plot_write(int32_t x, int32_t y);
void plot_point(int32_t x, int32_t y);
void plot_new_hline(int32_t xl, int32_t y, int32_t xr);
void plot_hline(int32_t a, int32_t y, int32_t b);
void plot_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1);

struct plot_line {
    int32_t x, y, bres, dx, dy, sx, sy, ex, ey;
};
void plot_gen_line(struct plot_line *l, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void plot_adv_line(struct plot_line *l);

/* shapes.c */
void plot_circle_outline(void);
void plot_circle_fill(void);
void plot_arc(void);
void plot_segment(void);
void plot_sector(void);
void plot_ellipse(int fill);

#endif
