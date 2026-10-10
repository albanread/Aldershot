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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/Video/Render/SprExtend: Sources/SprExtend, hdr.SprExtend).
 */

/* sprext.h -- what SpriteExtend's files share (spriteextend.c, scaled.c). */
#ifndef ROSGD_SPREXT_H
#define ROSGD_SPREXT_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

extern struct ros_module spriteextend_module;

enum { SA_END = 0, SA_NUMBER = 4, SA_FIRST = 8, SA_FREE = 12 };
enum {
    SP_NEXT = 0, SP_NAME = 4, SP_WIDTH = 16, SP_HEIGHT = 20, SP_LBIT = 24, SP_RBIT = 28,
    SP_IMAGE = 32, SP_TRANS = 36, SP_MODE = 40,
};

/* SpriteExtend's errors (hdr/NewErrors, its Messages) */
enum {
    SX_BADADDRESS = 0xFC, SX_NOWORK = 0x80, SX_NOTENOUGHROOM = 0x85, SX_DOESNTEXIST = 0x86,
    SX_DIVZERO = 0x169, SX_INVRC = 0x703, SX_NOIROOM = 0x706, SX_BADMODE = 0x708,
    SX_BADTRAN = 0x70B, SX_APPERR = 0x70E, SX_BADFLAGS = 0x712, SX_BADDEPTH = 0x716,
    SX_BADDATA = 0x71E, SX_NOGRSCL = 0x71F,
};
/* Every branch of it returns ros_error, which is never null, and the whole
 * module writes "return sx_err(...)" where an error is due.  Said so here:
 * the analyser will not inline a switch this size, so without it every
 * "return sx_err(...)" looks like a path that may carry on with nothing
 * set, and clang --analyze reported eleven of those. */
os_error *sx_err(uint32_t n) __attribute__((returns_nonnull));

/* The call in hand: R0 as given (spritecode), and the sprite found */
struct sx_call {
    uint32_t code, area, sp;
};
os_error *sx_find(struct ros_cpu *s, struct sx_call *c, int in_area);

/* The destination (readvduvars) and a mode's variables (readspritevars) */
struct sx_dest {
    int32_t xeig, yeig, orgx, orgy, gwl, gwb, gwr, gwt;
    uint32_t l2bpp, l2bpc, bpp, bpc, linelen, screen, ywind, flags, ncolour;
    uint32_t fg_ecf, bg_ecf;            /* the kernel's OR/EOR tables */
    uint32_t ecf_shift, ecf_yoffset;
};
void sx_read_dest(struct sx_dest *d);
struct sx_mode {
    uint32_t l2bpc, l2bpp, xeig, yeig, flags, ncolour;
};
int sx_read_mode(uint32_t mode, struct sx_mode *m);

uint32_t sx_swi(uint32_t n, uint32_t r[10], os_error **e);

/* scaled.c */
os_error *sx_scaled(struct ros_cpu *s, uint32_t reason);
os_error *sx_paint_char(struct ros_cpu *s);
os_error *sx_tile(struct ros_cpu *s);
os_error *sx_transformed(struct ros_cpu *s, uint32_t reason);

/* JPEG_PlotScaled's plot (jpeg.c): a decoded image in place of a sprite,
 * through the scaled-sprite path.  scaled.c works out the geometry, then
 * asks build() for the pixels as the original's decoder would have laid
 * them out for that plot (its options depend on the scale, the clipping
 * and the destination). */
struct sx_jgeom {
    const struct sx_dest *d;
    uint32_t in_x, in_y, xsize, ysize;  /* the first source pixel, the output's size */
    uint32_t xmag, ymag, xdiv, ydiv;    /* the scale, rationalised */
};
enum { SX_JRGB, SX_JNATIVE };           /* &0BGR words; the destination's own pixels */
struct sx_jsrc {
    uint32_t w, h;
    int32_t x, y;                       /* OS units */
    uint32_t scale[4];                  /* xmag, ymag, xdiv, ydiv */
    uint32_t flags;                     /* JPEG_PlotScaled's R5 */
    uint32_t map;                       /* its R6: a colour mapping descriptor */
    os_error *(*build)(struct sx_jsrc *j, const struct sx_jgeom *g);
    /* set by build() */
    const uint8_t *pix;                 /* top row first */
    size_t stride;
    uint32_t l2bpp;                     /* of the pixels in pix: 5, 4 or 3 */
    int kind;
    uint32_t ttr;                       /* a 32K table, or 0 */
    uint32_t dither_tc;                 /* dither_truecolour, as the decoder left it */
    int grey;                           /* jopt_GREY: no table below 4bpp, red's top bits */
    int grey_jpeg;                      /* one component: an 8bpp ordered dither of 3 bits */
};
os_error *sx_scaled_jpeg(struct sx_jsrc *j);

/* jpeg.c */
os_error *sx_jpeg_init(void);
void sx_jpeg_final(void);
os_error *sx_jpeg_bad_swi(struct ros_module *m, uint32_t offset);

#endif
