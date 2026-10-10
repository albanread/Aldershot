/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2008 Castle Technology Ltd
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
 * (Sources/Kernel: s.vdu.vdugrafg, s.vdu.vdugrafh, s.vdu.vdugrafi,
 * s.vdu.vdugrafj) and its headers (Sources/Programmer/HdrSrc: hdr.Sprite,
 * hdr.NewErrors).
 */
/* sprite.h -- what sprite.c, sprplot.c and sprout.c share. That is the
 * formats, the errors, the sprite's pixel geometry, and the header
 * builder. */
#ifndef ROSGD_VDU_SPRITE_H
#define ROSGD_VDU_SPRITE_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

/* The area control block and the sprite header (hdr/Sprite). */
enum { SA_END = 0, SA_NUMBER = 4, SA_FIRST = 8, SA_FREE = 12 };
enum {
    SP_NEXT = 0, SP_NAME = 4, SP_WIDTH = 16, SP_HEIGHT = 20, SP_LBIT = 24, SP_RBIT = 28,
    SP_IMAGE = 32, SP_TRANS = 36, SP_MODE = 40,
};

/* The kernel's sprite errors (hdr/NewErrors). */
enum {
    ERR_SPR_NOWORKSPACE = 0x80, ERR_SPR_NOTGRAPHICS = 0x81, ERR_SPR_NOROOM = 0x82,
    ERR_SPR_NOSPRITES = 0x83, ERR_SPR_NOTENOUGHROOM = 0x85, ERR_SPR_DOESNTEXIST = 0x86,
    ERR_SPR_BADFILE = 0x700, ERR_SPR_NOROOMTOMERGE = 0x701, ERR_SPR_ROWCOL = 0x703,
    ERR_SPR_NOROOMTOINSERT = 0x706, ERR_SPR_EXISTS = 0x707, ERR_SPR_BADMODE = 0x708,
    ERR_SPR_BADREASON = 0x709, ERR_SPR_BADSAVEAREA = 0x710, ERR_SPR_ISDEST = 0x711,
    ERR_SPR_NOMASK = 0x718, ERR_SPR_BADDPI = 0x719,
};
os_error *spr_err(uint32_t n);

/* SetupSprModeData */
struct spr_mode {
    uint32_t flags, l2bpc, l2bpp, xshft, npix, bpc, read_nc, write_nc, ncolour, xeig, yeig;
};
os_error *spr_mode(uint32_t m, struct spr_mode *d);
uint32_t spr_type(uint32_t m);
uint32_t spr_type_bpp(uint32_t type);
void spr_mask_width(uint32_t sp, uint32_t *words, uint32_t *lastbit);
int spr_geometry_ok(uint32_t sp);

/* A header in the making (SGet*). */
struct spr_get {
    uint32_t mode, pal, image, size, lbit, rbit;
    int32_t width, height;
    int32_t top_margin, bot_margin, lw_margin, rw_margin, top_row, clip_l;
    uint32_t lb_margin, rb_margin;
    struct spr_mode d;
};
os_error *spr_sanitize_mode(uint32_t *mode);
os_error *spr_pre_header(struct spr_get *g, uint32_t pal, int32_t l, int32_t b, int32_t r,
                         int32_t t);
void spr_write_header(uint32_t sp, const struct spr_get *g, const uint8_t key[12]);
void spr_write_palette(uint32_t sp, const struct spr_mode *d);
os_error *spr_create_header(uint32_t area, const struct spr_get *g, const uint8_t key[12],
                            uint32_t *sp);

void spr_get_name(uint32_t p, uint8_t key[12]);
uint32_t spr_find_key(uint32_t area, const uint8_t key[12]);
uint32_t spr_find(uint32_t area, uint32_t name);
void spr_move(uint32_t to, uint32_t from, uint32_t bytes);
os_error *spr_extend(uint32_t area, uint32_t sp, uint32_t n);
void spr_remove_words(uint32_t area, uint32_t sp, uint32_t n, uint32_t off);
void spr_remove_lh_wastage(uint32_t area, uint32_t sp);
void spr_delete(uint32_t area, uint32_t sp);

/* sprplot.c */
os_error *spr_get_sprite(struct ros_cpu *s, uint32_t reason, uint32_t area);
os_error *spr_put(struct ros_cpu *s, uint32_t reason, uint32_t sp);
os_error *spr_plot_mask(struct ros_cpu *s, uint32_t reason, uint32_t sp);
os_error *spr_screen_save(struct ros_cpu *s);
os_error *spr_screen_load(struct ros_cpu *s);

/* sprout.c */
os_error *spr_switch_output(struct ros_cpu *s, uint32_t reason, uint32_t area, uint32_t sp);

#endif
