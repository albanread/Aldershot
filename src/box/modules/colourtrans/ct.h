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
 * (Sources/Video/Render/Colours: s.Header, s.Commons, hdr.ColourTran).
 */

/* ct.h: what ColourTrans's files share. */
#ifndef ROSGD_CT_H
#define ROSGD_CT_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

extern struct ros_module colourtrans_module;

/* Mode flags (VduExt). */
enum {
    MF_FULLPALETTE = 0x80, MF_64K = 0x80, MF_GREYSCALE = 0x200, MF_RGB = 0x4000,
    MF_ALPHA = 0x8000,
};

/* Errors. Each value is the error number of the token in the UK Messages
 * file. */
enum {
    CT_BADMODE = 0x19, CT_CANTKILL = 0x103, CT_BADSWI = 0x110, CT_BADCALIB = 0xA00,
    CT_CONVOVER = 0xA01, CT_BADHSV = 0xA02, CT_SWITCHED = 0xA03, CT_BADMISCOP = 0xA04,
    CT_BADFLAGS = 0xA05, CT_BUFFOVER = 0xA06, CT_BADDEPTH = 0xA07,
};
os_error *ct_err(uint32_t n);

/* How a palette is matched. These are the routines in the original's
 * routinetable (s/Commons). */
enum ct_rc {
    RC_SIMPLE, RC_CLEVER8, RC_GREY8,
    RC_4444_TBGR, RC_4444_TRGB, RC_4444_ABGR, RC_4444_ARGB,
    RC_1555_TBGR, RC_1555_TRGB, RC_1555_ABGR, RC_1555_ARGB,
    RC_565_BGR, RC_565_RGB,
    RC_8888_TBGR, RC_8888_TRGB, RC_8888_ABGR, RC_8888_ARGB,
};

/* The result of build_colours: the table (up to 256 words of the form
 * &BBGGRRxx) and the routines that match against it. l2bpp is the original's
 * BuildColoursL2BPP. */
struct ct_pal {
    enum ct_rc rc;
    uint32_t n, l2bpp;
    uint32_t t[256];
};

/* The workspace, as laid out in s/Header. */
struct ct_cache {
    uint32_t key, colour, gcol, pat[4];
    uint8_t pvalid;
};
struct ct_ws {
    int valid;
    struct ct_cache cache[64];
    int cache_empty;                    /* 0 means the cache is known to be empty */
    uint32_t c_flags, c_l2bpp, c_l2nc;  /* CachedModeFlags, CachedL2BPP, CachedL2NColour */
    uint32_t palette_at, palette_copy, palette_flags;
    int cur_valid;
    struct ct_pal cur;                  /* CurrentPalette and PaletteCache* */
    uint32_t loading;                   /* ColourErrorLoading: &BlGlRl00 */
    uint32_t calib, calib_new;          /* Calibration_ptr (in the RMA), Calibration_newtable */
    uint32_t pattern_temp;              /* PatternTemp, 8 words in the RMA */
    uint32_t scratch;                   /* the module's part of ScratchSpace, in the RMA */
    uint32_t grey8;                     /* grey8bpp_palette: 256 words in the RMA */
    int persist;
};
extern struct ct_ws ct;

/* Palette flags for getpalentry. */
enum { PF_BRAINDAMAGED = 1, PF_DOUBLE = 2 };

uint32_t ct_swi(uint32_t n, uint32_t r[10], os_error **e);
int ct_mode_var(uint32_t mode, uint32_t var, uint32_t *v);  /* returns 0, or 1 if C is set */

/* match.c */
extern const uint32_t ct_modetwofivesix[16], ct_hardbits[16];
const uint32_t *ct_defpal(uint32_t l2bpp, uint32_t *n);
os_error *ct_build(uint32_t mode, uint32_t palette, struct ct_pal *p);
uint32_t ct_best(const struct ct_pal *p, uint32_t c);
uint32_t ct_worst(const struct ct_pal *p, uint32_t c);
uint32_t ct_cn2g(uint32_t n), ct_g2cn(uint32_t g);
void ct_getpalentry(uint32_t pal, uint32_t flags, uint32_t i, uint32_t *w1, uint32_t *w2);
void ct_my_read_palette(uint32_t i, uint32_t *w1, uint32_t *w2);
void ct_init_cache(void);
struct ct_cache *ct_lookup(uint32_t c);
struct ct_cache *ct_write_cache(uint32_t colour, uint32_t gcol, uint32_t phys);

/* models.c: calibration, CIE, HSV, CMYK and the files. */
uint32_t ct_convert_screen_colour(uint32_t c);
os_error *ct_models_swi(struct ros_cpu *s, uint32_t n);

/* dither.c */
void ct_pattern(struct ct_cache *e, uint32_t d, int for_setgcol);

/* tables.c */
os_error *ct_generate_table(struct ros_cpu *s, uint32_t flags);
void ct_free_tables(void);

/* fontcol.c */
os_error *ct_font_colours(struct ros_cpu *s, int set);

#endif
