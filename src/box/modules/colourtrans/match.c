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
 * (Sources/Video/Render/Colours: s.Commons, s.Enhanced, s.Palettes, s.Cache).
 */

/* match.c: ColourTrans's palettes and colour matching.
 *
 * This file follows Colours' s/Commons, s/Enhanced, s/Palettes and s/Cache.
 * It has three parts.
 *
 *   - build_colours finds the palette of a mode and decides how it is
 *     matched. The palette is the current one read through PaletteV, or
 *     the one in the sprite that output is switched to, or a table given
 *     by the caller, or the default.
 *   - Matching. SimpleMatch takes the weighted squared error over the
 *     table, working from the top down. For the default 256-colour palette
 *     the code uses the four-tint lookup (Clever8BPP). For the default grey
 *     ramp it uses a division (Grey8BPP). At 16 and 32 bpp it packs the
 *     pixel from the colour's top bits. The "worst" forms find the
 *     furthest colour. They have quirks of the original, which are noted
 *     where they occur.
 *   - The colour cache has 64 entries. A hash of the colour gives the
 *     entry, so the cache is direct mapped.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "ct.h"

/* ---- default palettes (defpals) ------------------------------------------------------------ */

static const uint32_t def1[2] = { 0x00000000, 0xFFFFFF00 };
static const uint32_t def2[4] = { 0x00000000, 0x0000FF00, 0x00FFFF00, 0xFFFFFF00 };
static const uint32_t def4[16] = {
    0x00000000, 0x0000FF00, 0x00FF0000, 0x00FFFF00, 0xFF000000, 0xFF00FF00, 0xFFFF0000, 0xFFFFFF00,
    0x00000000, 0x0000FF00, 0x00FF0000, 0x00FFFF00, 0xFF000000, 0xFF00FF00, 0xFFFF0000, 0xFFFFFF00,
};
const uint32_t ct_modetwofivesix[16] = {
    0x00000000, 0x10101000, 0x20202000, 0x30303000, 0x00004000, 0x10105000, 0x20206000, 0x30307000,
    0x40000000, 0x50101000, 0x60202000, 0x70303000, 0x40004000, 0x50105000, 0x60206000, 0x70307000,
};
const uint32_t ct_hardbits[16] = {
    0x0, 0x8000, 0x400000, 0x408000, 0x800000, 0x808000, 0xC00000, 0xC08000,
    0x80000000, 0x80008000, 0x80400000, 0x80408000, 0x80800000, 0x80808000, 0x80C00000, 0x80C08000,
};

const uint32_t *ct_defpal(uint32_t l2bpp, uint32_t *n)
{
    switch (l2bpp) {
    case 0: *n = 2; return def1;
    case 1: *n = 4; return def2;
    case 2: *n = 16; return def4;
    default: *n = 16; return ct_modetwofivesix;
    }
}

/* The standard 256 colours. This is buildfasttable's expansion of the
 * default palette. */
static uint32_t default256(uint32_t i)
{
    uint32_t v = (ct_modetwofivesix[i & 15] & 0x70307000) | ct_hardbits[i >> 4];
    return v | v >> 4;
}

/* ---- reading palettes ---------------------------------------------------------------------- */

/* getpalentry: entry i of the table at pal, which is an arena address. */
void ct_getpalentry(uint32_t pal, uint32_t flags, uint32_t i, uint32_t *w1, uint32_t *w2)
{
    uint32_t j = flags & PF_BRAINDAMAGED ? i & 15 : i;
    if (flags & PF_DOUBLE)
        *w1 = ros_ld32(pal + 8 * j), *w2 = ros_ld32(pal + 8 * j + 4);
    else
        *w1 = *w2 = ros_ld32(pal + 4 * j);
    if (flags & PF_BRAINDAMAGED) {
        uint32_t *w[2] = { w1, w2 };
        for (int k = 0; k < 2; k++) {
            uint32_t v = (*w[k] & ~0x80C08000u) | (i & 0x80) << 24 | (i & 0x60) << 17 |
                         (i & 0x10) << 11;
            v &= 0xF0F0F000u;
            *w[k] = v | v >> 4;
        }
    }
}

#define PALETTEV 0x23u

/* my_read_palette. It reads an entry from the sprite that output goes to,
 * or from PaletteV if output is not going to a sprite. */
void ct_my_read_palette(uint32_t i, uint32_t *w1, uint32_t *w2)
{
    if (ct.palette_at) {
        ct_getpalentry(ct.palette_at, ct.palette_flags, i, w1, w2);
        return;
    }
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = i, s.r[1] = 16, s.r[4] = 1;
    ros_vector_call(PALETTEV, &s);
    if (s.r[4] == 0) {
        *w1 = s.r[2], *w2 = s.r[3];
        return;
    }
    uint32_t r[10] = { i, 16 };
    os_error *e;
    ct_swi(XOS_ReadPalette, r, &e);
    *w1 = (r[2] & 0xF0F0F000u) | (r[2] & 0xF0F0F000u) >> 4, *w2 = r[3];
}

/* buildcurrent. It tries a PaletteV BulkRead of the first flash state. If
 * that fails it reads entry by entry, starting from the top. */
static void read_current(uint32_t n, uint32_t *t)
{
    if (!ct.palette_at) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = n | 17u << 24, s.r[2] = ct.scratch, s.r[3] = 0, s.r[4] = 7;
        ros_vector_call(PALETTEV, &s);
        if (s.r[4] == 0) {
            memcpy(t, ros_ptr(ct.scratch), 4 * n);
            return;
        }
    }
    for (uint32_t i = n; i--;) {
        uint32_t w2;
        ct_my_read_palette(i, &t[i], &w2);
    }
}

/* ---- build_colours ------------------------------------------------------------------------- */

static void choose(struct ct_pal *p, uint32_t flags)
{
    p->rc = RC_SIMPLE;
    if (p->l2bpp != 3 || ct.calib)
        return;
    if (!(flags & MF_FULLPALETTE)) {
        for (uint32_t i = 0; i < 16; i++)
            if ((p->t[i] & 0x70307000u) != ct_modetwofivesix[i])
                return;
        p->rc = RC_CLEVER8;
    } else if (flags & MF_GREYSCALE) {
        for (uint32_t i = 0; i < 256; i++)
            if (p->t[i] != i * 0x01010100u)
                return;
        p->rc = RC_GREY8;
    } else {
        for (uint32_t i = 0; i < 256; i++)
            if (p->t[i] != default256(i))
                return;
        p->rc = RC_CLEVER8;
    }
}

static void big(struct ct_pal *p, uint32_t nc, uint32_t f)
{
    enum ct_rc rc = RC_8888_TBGR;
    if (nc == 0xFFFFFF)
        f &= ~(uint32_t)MF_ALPHA;
    if (nc == 0xFFFF)
        rc = f & MF_64K ? RC_565_BGR : RC_1555_TBGR;
    if (nc == 0xFFF)
        rc = RC_4444_TBGR;
    if (rc != RC_565_BGR && (f & MF_ALPHA))
        rc += 2;
    if (f & MF_RGB)
        rc += 1;
    p->rc = rc;
}

os_error *ct_build(uint32_t mode, uint32_t palette, struct ct_pal *p)
{
    int current = mode == 0xFFFFFFFFu && palette == 0xFFFFFFFFu;
    if (current && ct.cur_valid) {
        *p = ct.cur;
        return NULL;
    }
    uint32_t keep_at = ct.palette_at;
    if (mode != 0xFFFFFFFFu && palette == 0xFFFFFFFFu)
        ct.palette_at = 0, palette = 0;    /* use the mode's default, not the current palette */
    uint32_t l2 = 0, flags = 0, nc = 0;
    if (mode != 0xFFFFFFFFu && mode >= 256 && (mode & 1)) {     /* a sprite mode word */
        ct_mode_var(mode, 9, &l2);
        if (l2 > 3)
            ct_mode_var(mode, 0, &flags);
        ct_mode_var(mode, 3, &nc);
    } else if (ct_mode_var(mode, 9, &l2) || ct_mode_var(mode, 0, &flags) ||
               ct_mode_var(mode, 3, &nc)) {
        ct.palette_at = keep_at;
        return ct_err(CT_BADMODE);
    }
    p->l2bpp = l2;
    if (l2 > 3) {
        p->n = 0;
        big(p, nc, flags);
    } else {
        uint32_t n = 1u << (1u << l2);
        p->n = n;
        if (palette == 0xFFFFFFFFu) {
            read_current(n, p->t);
        } else if (l2 == 3 && !(flags & MF_FULLPALETTE)) {     /* buildfasttable */
            uint32_t base[16], dn;
            if (palette) {
                for (int i = 0; i < 16; i++)
                    base[i] = ros_ld32(palette + 4u * (uint32_t)i);
            } else {
                memcpy(base, ct_defpal(3, &dn), sizeof base);
            }
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t v = (base[i & 15] & 0x70307000u) | ct_hardbits[i >> 4];
                p->t[i] = v | v >> 4;
            }
        } else if (palette) {
            for (uint32_t i = 0; i < n; i++)
                p->t[i] = ros_ld32(palette + 4 * i);
        } else if (l2 < 3) {
            uint32_t dn;
            memcpy(p->t, ct_defpal(l2, &dn), 4 * n);
        } else {
            /* This is 8 bpp with a full palette and the default palette.
             * The original copies 256 words from modetwofivesix on. Only 16
             * of them are palette. After those come hardmode_hardbits and
             * then the original's own code.
             * Here the first 32 words are copied, and the standard 256
             * colours fill the rest. */
            for (uint32_t i = 0; i < 256; i++)
                p->t[i] = i < 16 ? ct_modetwofivesix[i] : i < 32 ? ct_hardbits[i - 16] :
                                                                 default256(i);
        }
        choose(p, flags);
    }
    ct.palette_at = keep_at;
    if (current)
        ct.cur = *p, ct.cur_valid = 1;
    return NULL;
}

/* ---- matching -------------------------------------------------------------------------------- */

static uint32_t err3(uint32_t c, uint32_t e, uint32_t L)
{
    int32_t dr = (int32_t)((c >> 8) & 255) - (int32_t)((e >> 8) & 255);
    int32_t dg = (int32_t)((c >> 16) & 255) - (int32_t)((e >> 16) & 255);
    int32_t db = (int32_t)(c >> 24) - (int32_t)(e >> 24);
    return (uint32_t)(db * db) * (L >> 24) + (uint32_t)(dg * dg) * ((L >> 16) & 255) +
           (uint32_t)(dr * dr) * ((L >> 8) & 255);
}

/* Find256_Table[c]. Byte t is the nearest level of tint t to c. */
static uint32_t find256(uint32_t c)
{
    uint32_t w = 0;
    for (uint32_t t = 0; t < 4; t++) {
        int32_t x = 0x20 - 16 * (int32_t)t + (int32_t)c - (int32_t)(c >> 4);
        x = x < 0 ? 0 : x > 255 ? 255 : x;
        uint32_t v = ((uint32_t)x & 0xC0) + 16 * t;
        w |= (v + (v >> 4)) << (8 * t);
    }
    return w;
}

/* Find256 and Convert24Number. The result is a colour number with the bits
 * B G G R B R T T. */
static uint32_t clever8(uint32_t c, int worst)
{
    uint32_t L = ct.loading, r = (c >> 8) & 255, g = (c >> 16) & 255, b = c >> 24;
    uint32_t R = find256(r), G = find256(g), B = find256(b), E = worst ? 0 : 0xFFFFFFFFu;
    uint32_t col = 0;
    for (int t = 3; t >= 0; t--) {
        uint32_t Rc = (R >> (8 * t)) & 255, Gc = (G >> (8 * t)) & 255, Bc = (B >> (8 * t)) & 255;
        int32_t dr = (int32_t)r - (int32_t)Rc, dg = (int32_t)g - (int32_t)Gc;
        int32_t db = (int32_t)b - (int32_t)Bc;
        uint32_t e = (uint32_t)(dr * dr) * ((L >> 8) & 255) + (uint32_t)(dg * dg) * ((L >> 16) & 255) +
                     (uint32_t)(db * db) * (L >> 24);
        if (worst ? e >= E : e < E)
            E = e, col = Bc << 24 | Gc << 16 | Rc << 8;
    }
    return ((col & 0x80000000u) >> 24) | ((col & 0xC00000) >> 17) | ((col & 0x8000) >> 11) |
           ((col & 0x40000000) >> 27) | ((col & 0x7000) >> 12);
}

static uint32_t grey8(uint32_t c, int worst)
{
    uint32_t L = ct.loading;
    uint32_t rl = (L >> 8) & 255, gl = (L >> 16) & 255, bl = L >> 24;
    uint32_t W = rl + gl + bl;
    uint32_t S = (c >> 24) * bl + ((c >> 8) & 255) * rl + ((c >> 16) & 255) * gl + (W >> 1);
    if (worst)
        return (int32_t)S >= (int32_t)(W << 7) ? 0 : 255;
    return W ? S / W : 0;   /* the original's DivRem by 0 never returns */
}

static uint32_t pack(uint32_t c, enum ct_rc rc)
{
    uint32_t r = (c >> 8) & 255, g = (c >> 16) & 255, b = c >> 24;
    switch (rc) {
    case RC_4444_TBGR: return r >> 4 | (g >> 4) << 4 | (b >> 4) << 8;
    case RC_4444_TRGB: return b >> 4 | (g >> 4) << 4 | (r >> 4) << 8;
    case RC_4444_ABGR: return (r >> 4 | (g >> 4) << 4 | (b >> 4) << 8) | 0xF000;
    case RC_4444_ARGB: return (b >> 4 | (g >> 4) << 4 | (r >> 4) << 8) | 0xF000;
    case RC_1555_TBGR: return r >> 3 | (g >> 3) << 5 | (b >> 3) << 10;
    case RC_1555_TRGB: return b >> 3 | (g >> 3) << 5 | (r >> 3) << 10;
    case RC_1555_ABGR: return (r >> 3 | (g >> 3) << 5 | (b >> 3) << 10) | 0x8000;
    case RC_1555_ARGB: return (b >> 3 | (g >> 3) << 5 | (r >> 3) << 10) | 0x8000;
    case RC_565_BGR: return r >> 3 | (g >> 2) << 5 | (b >> 3) << 11;
    case RC_565_RGB: return b >> 3 | (g >> 2) << 5 | (r >> 3) << 11;
    case RC_8888_TBGR: return c >> 8;
    case RC_8888_TRGB: return r << 16 | g << 8 | b;
    case RC_8888_ABGR: return c >> 8 | 0xFF000000u;
    default: return r << 16 | g << 8 | b | 0xFF000000u;
    }
}

uint32_t ct_best(const struct ct_pal *p, uint32_t c)
{
    if (p->rc >= RC_4444_TBGR)
        return pack(c, p->rc);
    if (p->rc == RC_GREY8)
        return grey8(c, 0);
    if (ct.calib)
        c = ct_convert_screen_colour(c);
    if (p->rc == RC_CLEVER8)
        return clever8(c, 0);
    uint32_t E = 0xFFFFFFFFu, idx = 0;
    for (uint32_t i = p->n; i--;) {
        uint32_t e = err3(c, p->t[i], ct.loading);
        if (e <= E)
            E = e, idx = i;
    }
    return idx;
}

uint32_t ct_worst(const struct ct_pal *p, uint32_t c)
{
    if (p->rc >= RC_4444_TBGR)
        return pack(~c, p->rc);
    if (p->rc == RC_GREY8)
        return grey8(c, 1);
    if (ct.calib)
        c = ct_convert_screen_colour(c);
    if (p->rc == RC_CLEVER8)
        return clever8(c, 1);
    uint32_t E = 0, idx = c >> 2;           /* if all errors are 0 the result is the colour >> 2, as in the original */
    for (uint32_t i = p->n; i--;) {
        uint32_t e = err3(c, p->t[i], ct.loading);
        if (e > E)
            E = e, idx = i;
    }
    return idx;
}

/* ---- GCOLs and colour numbers ---------------------------------------------------------------- */

static const uint8_t cn2g[64] = {
    0x00, 0x01, 0x10, 0x11, 0x02, 0x03, 0x12, 0x13, 0x04, 0x05, 0x14, 0x15, 0x06, 0x07, 0x16, 0x17,
    0x08, 0x09, 0x18, 0x19, 0x0A, 0x0B, 0x1A, 0x1B, 0x0C, 0x0D, 0x1C, 0x1D, 0x0E, 0x0F, 0x1E, 0x1F,
    0x20, 0x21, 0x30, 0x31, 0x22, 0x23, 0x32, 0x33, 0x24, 0x25, 0x34, 0x35, 0x26, 0x27, 0x36, 0x37,
    0x28, 0x29, 0x38, 0x39, 0x2A, 0x2B, 0x3A, 0x3B, 0x2C, 0x2D, 0x3C, 0x3D, 0x2E, 0x2F, 0x3E, 0x3F,
};
static const uint8_t g2cn[64] = {
    0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0C, 0x0D, 0x10, 0x11, 0x14, 0x15, 0x18, 0x19, 0x1C, 0x1D,
    0x02, 0x03, 0x06, 0x07, 0x0A, 0x0B, 0x0E, 0x0F, 0x12, 0x13, 0x16, 0x17, 0x1A, 0x1B, 0x1E, 0x1F,
    0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x30, 0x31, 0x34, 0x35, 0x38, 0x39, 0x3C, 0x3D,
    0x22, 0x23, 0x26, 0x27, 0x2A, 0x2B, 0x2E, 0x2F, 0x32, 0x33, 0x36, 0x37, 0x3A, 0x3B, 0x3E, 0x3F,
};

/* The original makes no range check. Past 255 it reads the next table and
 * then its code. Here the index wraps. */
uint32_t ct_cn2g(uint32_t n)
{
    return (n & 3) | (uint32_t)cn2g[(n >> 2) & 63] << 2;
}

uint32_t ct_g2cn(uint32_t g)
{
    return (g & 3) | (uint32_t)g2cn[(g >> 2) & 63] << 2;
}

/* ---- the colour cache ------------------------------------------------------------------------ */

static unsigned hash(uint32_t c)
{
    return ((c >> 9) ^ (c >> 19) ^ (c >> 29)) & 63;
}

struct ct_cache *ct_lookup(uint32_t c)
{
    struct ct_cache *e = &ct.cache[hash(c)];
    return e->key == c >> 8 ? e : NULL;
}

struct ct_cache *ct_write_cache(uint32_t colour, uint32_t gcol, uint32_t phys)
{
    struct ct_cache *e = &ct.cache[hash(phys)];
    e->key = phys >> 8, e->colour = colour, e->gcol = gcol, e->pvalid = 0xFF;
    ct.cache_empty = 0xFF;
    return e;
}

/* InitCache. It reads the output's depth, flags and colours again and drops
 * the palette cache. If the colour cache held anything, it empties it and
 * issues Service_InvalidateCache. */
void ct_init_cache(void)
{
    uint32_t l2 = 0, f = 0, nc = 0;
    ct_mode_var(0xFFFFFFFFu, 9, &l2);
    if (ct_mode_var(0xFFFFFFFFu, 0, &f))
        f = 0;
    ct_mode_var(0xFFFFFFFFu, 3, &nc);
    ct.c_l2bpp = l2 & 255, ct.c_flags = f & 0xFFFF;
    uint32_t bits = 0;
    for (uint32_t v = nc; v; v >>= 1)
        bits++;
    ct.c_l2nc = bits;
    ct.palette_copy = ct.palette_at;
    ct.cur_valid = 0;
    if (ct.cache_empty) {
        for (int i = 0; i < 64; i++)
            ct.cache[i].key = 0xFF000000u, ct.cache[i].pvalid = 0xFF;
        ct.cache_empty = 0;
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = 0x82;                          /* Service_InvalidateCache */
        ros_service_call(&s);
    }
}
