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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts02, s.Fonts01, s.Blending).
 */

/* colours.c: the Font Manager's colours.  This covers s/Fonts02's
 * setpalette, setoutputdata and correctaliascolours, and the SWIs of
 * s/Fonts01: Font_SetFontColours, Font_SetPalette, Font_ReadColourTable,
 * Font_SetColourTable, Font_CurrentRGB and Font_FutureRGB.
 *
 * Painting turns each of a character's 16 levels of grey into a pixel value
 * through outputdata. The value is held in the top <bpp> bits of each word.
 *
 * Below 8 bpp the table is built from logical colours, using the
 * thresholds. The colours are the background, and the foreground colours
 * counting up or down from forecolour. Font_SetPalette also programs the
 * palette itself at these depths.
 *
 * At 8 bpp and above the table is built from a pseudo-palette. There is one
 * entry for each foreground colour 0 to 15. Font_SetPalette fills each
 * with ColourTrans's colour numbers for the steps between two RGB colours.
 *
 * ColourTrans may also supply the whole table (Font_SetColourTable).
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "fm.h"

enum { SETOUT_INVALID = 0, SETOUT_VALID = 1, SETOUT_BADBLENDING = 0x80 };
#define INVALID_RGB 0x80u
#define MAXPAL 16
#define PALETTEV 0x23u

uint32_t fm_outputdata[16], fm_outputmask, fm_rubdata;
uint8_t fm_outputvalid;
uint8_t fm_bpp = 1, fm_ppw = 32;        /* bitsperpixel and Pixelsperword */
int fm_inscanstring;
uint32_t fm_switch_buffer;              /* Font_SwitchOutputToBuffer's buffer, or 0 */
uint32_t fm_switch_flags;               /* its flags.  Bit 31 means enabled. */
uint32_t fm_switch_fore, fm_switch_back;    /* the RGB colours it draws its objects in */
uint32_t fm_plottype;

struct pseudo {                         /* psb_: a pseudo-palette entry */
    uint32_t aliascolours, back, fore;
    uint32_t table[16];
};
static struct pseudo *pseudo[MAXPAL];

/* The depth part of setmodedata.  It invalidates the table if the depth
 * changed. */
void fm_depth(int32_t log2bpp)
{
    uint8_t bpp = (uint8_t)(1u << log2bpp);
    if (bpp != fm_bpp)
        fm_outputvalid = SETOUT_INVALID;
    fm_bpp = bpp;
    fm_ppw = (uint8_t)(32 >> log2bpp);
    fm_outputmask = bpp == 1 ? 0x80000000u : (uint32_t)((int32_t)0x80000000 >> (bpp - 1));
}

/* ---- the palette ---------------------------------------------------------------------------- */

/* getoutputfromRGB: the colour number, placed at the top of a word */
static os_error *output_from_rgb(uint32_t rgb, uint32_t *out)
{
    uint32_t r[10] = { rgb };
    os_error *e;
    fm_swi(XColourTrans_ReturnColourNumber, r, &e);
    if (!e) {
        *out = fm_bpp >= 32 ? r[0] : r[0] << (32 - fm_bpp);
        return NULL;
    }
    if ((e->errnum & 0xFFFFFF) != FE_NOSUCHSWI)
        return e;
    uint32_t v = 0;                     /* no ColourTrans: use the 256-colour palette's own numbering */
    if (rgb & 0x80000000u) v |= 0x80000000u;
    if (rgb & 0x00800000u) v |= 0x40000000u;
    if (rgb & 0x00400000u) v |= 0x20000000u;
    if (rgb & 0x00008000u) v |= 0x10000000u;
    if (rgb & 0x40000000u) v |= 0x08000000u;
    if (rgb & 0x00004000u) v |= 0x04000000u;
    uint32_t t = (rgb & 0x30000000u) + ((rgb & 0x00300000u) << 9) + ((rgb & 0x00003000u) << 16);
    t = (t + 0x20000000u) & 0xC0000000u;
    *out = v | t >> 6;
    return NULL;
}

/* storeoutput: store value v for colour k of the table, over the levels
 * that the thresholds give it */
static void store_output(uint32_t *table, const uint8_t *thresh, uint32_t k, uint32_t v)
{
    uint32_t last = thresh[k];
    if (last > 16)
        last = 16;
    uint32_t first = k ? thresh[k - 1] : 0;
    for (uint32_t i = first; i < last; i++)
        table[i] = v;
}

/* getstepvalues: c gets the colour of fore and st the step towards back.
 * Each gun is held shifted left by 16. */
static void steps(uint32_t n, uint32_t back, uint32_t fore, int32_t c[3], int32_t st[3])
{
    int32_t d = (int32_t)n + 1;
    int32_t b[3] = { (int32_t)((back & 0xFF00) << 8), (int32_t)(back & 0xFF0000),
                     (int32_t)((back & 0xFF000000u) >> 8) };
    c[0] = (int32_t)((fore & 0xFF00) << 8), c[1] = (int32_t)(fore & 0xFF0000);
    c[2] = (int32_t)((fore & 0xFF000000u) >> 8);
    for (int k = 0; k < 3; k++)
        st[k] = fm_divide(b[k] - c[k], d);
}

static uint32_t gun(int32_t v)          /* GetGun */
{
    v >>= 16;
    return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
}

static uint32_t rgb_of(const int32_t c[3])      /* getRGBfromR3R4R5 */
{
    return gun(c[0]) << 8 | gun(c[1]) << 16 | gun(c[2]) << 24;
}

/* setrealpalette: call PaletteV, or OS_Word 12 if nothing claims it */
static os_error *real_palette(uint32_t rgb, uint32_t colour)
{
    os_error *e;
    uint32_t r[10] = { colour, 16, rgb, rgb, 2, 0, 0, 0, 0, PALETTEV };
    fm_swi(XOS_CallAVector, r, &e);
    if (e || r[4] == 0)
        return e;
    uint32_t blk = ros_vdu_scratch();
    ros_st8(blk, r[0] & 0xFF), ros_st8(blk + 1, r[1] & 0xFF);
    ros_st8(blk + 2, (r[2] >> 8) & 0xFF), ros_st8(blk + 3, (r[2] >> 16) & 0xFF);
    ros_st8(blk + 4, r[2] >> 24);
    uint32_t w[10] = { 12, blk };
    fm_swi(XOS_Word, w, &e);
    return e;
}

/* setpalette: takes back, fore, aliascolours and the two RGB colours */
static os_error *set_palette(uint32_t back, uint32_t fore, int32_t alias, uint32_t brgb,
                             uint32_t frgb)
{
    os_error *e = fm_mode_vars();
    if (e)
        return e;
    int32_t c[3], st[3];
    if (fm_bpp < 8) {
        if ((e = real_palette(brgb, back)))
            return e;
        uint32_t n = alias < 0 ? (uint32_t)-alias : (uint32_t)alias;
        int32_t dir = alias < 0 ? -1 : 1;
        uint32_t k = fore + (uint32_t)alias;
        steps(n, brgb, frgb, c, st);
        for (;;) {
            if ((e = real_palette(rgb_of(c), k)))
                return e;
            if (k == fore)
                return NULL;
            k -= (uint32_t)dir;
            for (int j = 0; j < 3; j++)
                c[j] += st[j];
        }
    }
    if (fore >= MAXPAL)
        return fm_err(FE_PALTOOBIG, NULL, NULL);
    struct pseudo *p = pseudo[fore];
    if (!p && !(p = pseudo[fore] = calloc(1, sizeof *p)))
        return fm_err(FE_NOROOM, NULL, NULL);
    uint32_t n = alias < 0 ? (uint32_t)-alias : (uint32_t)alias;
    p->aliascolours = n, p->back = brgb, p->fore = frgb;
    const uint8_t *thresh = &fm_thresholds[fm_thresh_offset(n)];
    uint32_t v;
    if ((e = output_from_rgb(brgb, &v)))
        return e;
    store_output(p->table, thresh, 0, v);
    if ((e = output_from_rgb(frgb, &v)))
        return e;
    store_output(p->table, thresh, n + 1, v);
    if (n == 0)
        return NULL;
    steps(n, brgb, frgb, c, st);
    for (uint32_t k = n; k; k--) {
        for (int j = 0; j < 3; j++)
            c[j] += st[j];
        if ((e = output_from_rgb(rgb_of(c), &v)))
            return e;
        store_output(p->table, thresh, k, v);
    }
    return NULL;
}

/* ---- the table ---------------------------------------------------------------------------------- */

/* correctaliascolours: the number of distinct colours, counted as the
 * original's check8 counts them.  It never compares words 6 and 7, or
 * words 14 and 15. */
static void correct_alias(void)
{
    int32_t n = 0;
    for (int k = 0; k < 16; k++) {
        if (k == 6 || k == 14 || k == 15)
            continue;
        if (fm_outputdata[k] != fm_outputdata[k + 1])
            n++;
    }
    n--;
    if (n < 0)
        n = 0;
    fm_current.acol = (uint8_t)n;       /* (the original tests the sign of a byte, so it is never negative) */
}

static os_error *set_output(void);

/* ---- blending (s/Blending: setblendingdata) ------------------------------------------------------ */

struct fm_blend fm_blend;
static uint32_t blend_plottype;
#define PAINT_BLENDED (1u << 11)
#define PAINT_BLENDSUPR (1u << 14)

/* checkblend: blending that was set up for other flags is out of date */
void fm_check_blend(void)
{
    if ((fm_plottype ^ blend_plottype) & (PAINT_BLENDED | PAINT_BLENDSUPR))
        fm_outputvalid |= SETOUT_BADBLENDING;
}

/* ColourConv: &BBGGRRxx to a true colour pixel, keeping the top bits of
 * each channel */
static uint32_t conv(uint32_t c, int rs, int rb, int gs, int gb, int bs, int bb)
{
    uint32_t r = (c >> 8 & 0xFF) >> (8 - rb), g = (c >> 16 & 0xFF) >> (8 - gb),
             b = (c >> 24) >> (8 - bb);
    return r << rs | g << gs | b << bs;
}

static os_error *set_blending(void)
{
    blend_plottype = fm_plottype;
    fm_blend.kind = BLEND_NONE;
    if (!(fm_plottype & PAINT_BLENDED) || fm_log2bpp > 5 || fm_log2bpp < 3)
        return NULL;
    int kind = fm_log2bpp == 5 ? BLEND_32 : fm_log2bpp == 4 ? BLEND_1555 : BLEND_8;
    if (kind == BLEND_1555) {
        if (fm_vdu.modeflags & 0x80)
            kind = BLEND_565;
        if ((int32_t)fm_vdu.ncolour < 4096)
            kind = BLEND_4444;
    }
    int mode = 0;
    if (fm_plottype & PAINT_BLENDSUPR) {
        mode = fm_vdu.modeflags & 0x8000 ? 2 : 1;
        if (kind != BLEND_4444 && kind != BLEND_32)
            return fm_err(FE_SUPREMACY, NULL, NULL);
    }
    uint32_t c = fm_current.rgb_f;
    if (fm_vdu.modeflags & 0x4000)                      /* RGB order: swap red and blue */
        c = (c & 0xFF0000) | ((c & ~0xFFu) >> 16 | (c & ~0xFFu) << 16);
    uint32_t fg;
    switch (kind) {
    case BLEND_32: fg = c >> 8; break;
    case BLEND_565: fg = conv(c, 0, 5, 5, 6, 11, 5); fg |= fg << 16; break;
    case BLEND_4444: fg = conv(c, 0, 4, 4, 4, 8, 4); fg |= fg << 16; break;
    default: fg = conv(c, 0, 5, 5, 5, 10, 5); fg |= fg << 16; break;
    }
    fm_blend.fg = fg;
    uint32_t a = 255 - (fm_current.rgb_f & 0xFF);
    fm_blend.fgalpha = a >= 128 ? a + 1 : a;
    fm_blend.mode = mode;
    if (kind == BLEND_8) {                              /* InverseTable_Calculate */
        uint32_t r[10] = { 0 };
        os_error *e;
        fm_swi(0x20000u | 0x4BF40u, r, &e);
        if (e)
            return NULL;                                /* (no blending without it) */
        fm_blend.ctable = r[0], fm_blend.itable = r[1];
    }
    fm_blend.kind = kind;
    return NULL;
}

static void rub(void)
{
    uint32_t v = fm_outputdata[0];
    for (unsigned k = fm_ppw; k; k--)
        v |= fm_bpp >= 32 ? 0 : v >> fm_bpp;
    fm_rubdata = v;
}

/* trysetoutputdata: call setoutputdata if the table is invalid */
os_error *fm_set_output(void)
{
    if (fm_outputvalid & SETOUT_BADBLENDING) {
        fm_outputvalid &= ~SETOUT_BADBLENDING;
        if (fm_outputvalid != SETOUT_INVALID)
            return set_blending();
    }
    if (fm_outputvalid != SETOUT_INVALID)
        return NULL;
    return set_output();
}

/* setoutputdata: always build the table from the current colours */
static os_error *set_output(void)
{
    if (fm_switch_buffer) {
        if (fm_current.rgb_a == INVALID_RGB)
            return fm_err(FE_BADRGB, NULL, NULL);
        return NULL;
    }
    if (fm_bpp >= 8) {
        uint8_t f = fm_current.fcol;
        struct pseudo *p = f < MAXPAL ? pseudo[f] : NULL;
        if (!p)
            return fm_err(FE_PALTOOBIG, NULL, NULL);
        fm_current.acol = (uint8_t)p->aliascolours;
        memcpy(fm_outputdata, p->table, sizeof fm_outputdata);
        correct_alias();
        if (fm_plottype & (1u << 11))           /* paint_blended */
            fm_current.acol = 14;
    } else {
        uint8_t a = fm_current.acol;
        unsigned k = a >= 128 ? 256u - a : a;
        const uint8_t *thresh = &fm_thresholds[fm_thresh_offset(k)];
        uint32_t ti = 0;
        for (uint32_t vi = 0; vi < 16; vi++) {
            if (vi >= thresh[ti])
                ti++;
            uint32_t c;
            if ((int32_t)ti - 1 < 0)
                c = fm_current.bcol;
            else
                c = a < 128 ? fm_current.fcol + (ti - 1) : fm_current.fcol - (ti - 1);
            uint32_t v = c >> fm_bpp | c << (32 - fm_bpp);
            fm_outputdata[vi] = v;
            if (v << fm_bpp)
                return fm_err(FE_NOTENOUGHBITS, NULL, NULL);
        }
    }
    fm_outputvalid = SETOUT_VALID;
    rub();
    return set_blending();
}

/* ---- the SWIs ------------------------------------------------------------------------------------ */

static void done(struct ros_cpu *s, os_error *e)
{
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

static os_error *check_alias(uint32_t r3)       /* checkaliascolours */
{
    return r3 + 14 <= 28 ? NULL : fm_err(FE_BADTRANBITS, NULL, NULL);
}

void fm_thunk_SetFontColours(struct ros_cpu *s)
{
    os_error *e = check_alias(s->r[3]);
    if (!e) {
        if (s->r[0])
            fm_currentfont = fm_futurefont = s->r[0] & 0xFF;
        fm_current.bcol = (uint8_t)s->r[1], fm_current.fcol = (uint8_t)s->r[2];
        fm_current.acol = (uint8_t)s->r[3];
        fm_outputvalid = SETOUT_INVALID;
        fm_current.rgb_a = INVALID_RGB;         /* invalidateRGB */
    }
    done(s, e);
}

void fm_thunk_SetPalette(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1], r2 = s->r[2], r4 = s->r[4], r5 = s->r[5];
    os_error *e = check_alias(s->r[3]);
    if (!e) {
        if (s->r[6] != 0x65757254) {            /* "True" */
            r1 &= 0x7F, r2 &= 0xFF;
            r4 &= 0xF0F0F000u, r5 &= 0xF0F0F000u;
            r4 |= r4 >> 4, r5 |= r5 >> 4;
        }
        fm_current.bcol = (uint8_t)r1, fm_current.fcol = (uint8_t)r2;
        fm_current.acol = (uint8_t)s->r[3];
        fm_outputvalid = SETOUT_INVALID;
        e = set_palette(r1, r2, (int32_t)s->r[3], r4, r5);
    }
    done(s, e);
}

/* VDU 23,25,&80+b,f,r,g,b,R,G,B: the colours set as Font_SetPalette sets
 * them, with the current aliascolours and the old 12-bit RGBs */
os_error *fm_vdu_palette(uint32_t q)
{
    uint32_t b = ros_ld8(q + 1) & 0x7F, f = ros_ld8(q + 2);
    fm_current.bcol = (uint8_t)b, fm_current.fcol = (uint8_t)f;
    fm_outputvalid = SETOUT_INVALID;
    uint32_t brgb = ros_ld8(q + 3) << 8 | ros_ld8(q + 4) << 16 | ros_ld8(q + 5) << 24;
    uint32_t frgb = ros_ld8(q + 6) << 8 | ros_ld8(q + 7) << 16 | ros_ld8(q + 8) << 24;
    brgb &= 0xF0F0F000u, frgb &= 0xF0F0F000u;
    brgb |= brgb >> 4, frgb |= frgb >> 4;
    return set_palette(b, f, (int8_t)fm_current.acol, brgb, frgb);
}

void fm_thunk_ReadColourTable(struct ros_cpu *s)
{
    os_error *e = fm_mode_vars();
    if (!e)
        e = set_output();               /* (not trysetoutputdata, because a table that was given is not read back) */
    if (!e) {
        if (s->r[0] == 0x44524F57) {            /* "WORD" */
            for (uint32_t k = 0; k < 16; k++)
                ros_st32(s->r[1] + 4 * k, fm_outputdata[k]);
        } else {
            for (uint32_t k = 0; k < 16; k++)
                ros_st8(s->r[1] + k, fm_bpp >= 32 ? fm_outputdata[k] : fm_outputdata[k] >> (32 - fm_bpp));
        }
    }
    done(s, e);
}

void fm_thunk_SetColourTable(struct ros_cpu *s)
{
    os_error *e = fm_mode_vars();
    if (e) {
        done(s, e);
        return;
    }
    if (!fm_inscanstring && s->r[1]) {
        for (uint32_t k = 0; k < 16; k++)
            fm_outputdata[k] = ros_ld32(s->r[1] + 4 * k);
        fm_outputvalid = SETOUT_VALID | SETOUT_BADBLENDING;
        correct_alias();
        rub();
    }
    fm_current.rgb_b = s->r[2], fm_current.rgb_f = s->r[3], fm_current.rgb_a = s->r[4];
    fm_switch_back = s->r[2], fm_switch_fore = s->r[3];
    if (s->r[0])
        fm_currentfont = s->r[0] & 0xFF;
    s->v = 0;
}

static void rgb(struct ros_cpu *s, const struct fm_colours *c, uint32_t font)
{
    if (c->rgb_a == INVALID_RGB) {
        ros_swi_fail(s, fm_err(FE_BADRGB, NULL, NULL));
        return;
    }
    s->r[0] = font, s->r[1] = c->rgb_b, s->r[2] = c->rgb_f, s->r[3] = c->rgb_a;
    s->v = 0;
}

void fm_thunk_CurrentRGB(struct ros_cpu *s) { rgb(s, &fm_current, fm_currentfont); }
void fm_thunk_FutureRGB(struct ros_cpu *s) { rgb(s, &fm_future, fm_futurefont); }
