/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2013 Castle Technology Ltd
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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts02, s.Blending, s.BlendingS).
 */

/* paint.c: painting strings (s/Fonts02's paintchars, paintchar, the
 * plotters, rub-out and underline) and the caret (paintcaret). It
 * implements Font_Paint and Font_Caret.
 *
 * The string is read as scan.c reads it, and each character's advance is
 * added as a scan adds it. Each character is placed at its pen position,
 * rounded down to a pixel. Its bitmap is chosen for the quarter pixel left
 * over. It is written straight into the screen memory that the VDU's
 * variables give, or into a sprite's memory when output goes to a sprite.
 * It is clipped to the graphics window. As the original does, it writes
 * levels 1 to 15 in their colours, leaves level 0 alone, and takes no
 * GCOL action. A rub-out box is filled with the background colour a word at
 * a time, running ahead of the text. An underline is drawn between the
 * characters' glue mid-points. Characters too big for the cache are drawn
 * through Draw.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fm.h"
#include "fontmanager.h"

#define BIGNUM 0x20000000

enum {
    PT_JUSTIFY = 1, PT_RUBOUT = 2, PT_OSUNITS = 1 << 4, PT_COORDBLK = 1 << 5, PT_MATRIX = 1 << 6,
    PT_LENGTH = 1 << 7, PT_FONT = 1 << 8, PT_RL = 1 << 10, PT_RESERVED = 0xFFFF8000u,
};

static int32_t add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

/* ---- the painting's state (workspace) ---------------------------------------------------------- */

static uint32_t endaddr;                /* paintendaddress */
static int32_t justifyx, justifyy;
static int32_t rub[4];                  /* rubx0, ruby0, rubx1, ruby1, in pixels and inclusive */
static int32_t ruboutx;                 /* how far the rub-out has got, or BIGNUM for none */
static uint8_t setruboutflag;           /* 0 if setrubout is still to be done */
static uint32_t ruboutptr, maxrubptr, startrubmask, endrubmask, oldrubdata;
static uint8_t ruboutcount;
static int32_t rubouth;
static int32_t xsize, rowcount;         /* the character's box */
static int32_t lastxcoord, ul_nextxcoord, saved_nextx, saved_nexty;
static uint8_t ul_top, ul_midheight, ul_bot;
static int32_t ul_xcoord, ul_ycoord;
static int32_t switch_ulxcoord, switch_ulycoord, switch_ultop, switch_ulheight, switch_nextxcoord;
static uint32_t changedbox;             /* OS_ChangedBox's block */
/* output to a buffer: the rectangle objects under way and their colours */
static uint32_t switch_underline, switch_rubout, switch_lastfore, switch_lastback;
static int32_t switch_lastrubx1, switch_ullastx;

static int32_t scalexco(int32_t p) { return (int32_t)((uint32_t)(p * fm_xscalefactor) << fm_xeig); }
static int32_t scaleyco(int32_t p) { return (int32_t)((uint32_t)(p * fm_yscalefactor) << fm_yeig); }

/* ---- the screen ------------------------------------------------------------------------------------- */

/* getaddr: the word holding pixel (x, y). *left is the number of pixels
 * from x to the end of the word. */
static uint32_t getaddr(int32_t x, int32_t y, uint32_t *left)
{
    uint32_t ppw = fm_ppw;
    if (left)
        *left = ppw - ((uint32_t)x & (ppw - 1));
    return fm_vdu.scrtop + fm_vdu.linelen * (uint32_t)(fm_vdu.ywindlimit - y) +
           ((uint32_t)(x >> (5 - fm_log2bpp)) << 2);
}

/* extendmask: a mask of the top n pixels of a word */
static uint32_t extendmask(uint32_t n)
{
    uint32_t m = fm_outputmask;
    while (--n)
        m |= m >> fm_bpp;
    return m;
}

/* Puts a pixel's bits into its word, taking the value from the top of a word. */
static void put_pixel(int32_t x, int32_t y, uint32_t top)
{
    uint32_t a = getaddr(x, y, NULL), sh = ((uint32_t)x & (fm_ppw - 1)) * fm_bpp;
    uint32_t mask = fm_bpp == 32 ? 0xFFFFFFFFu : ((1u << fm_bpp) - 1) << sh;
    uint32_t v = fm_bpp == 32 ? top : (top >> (32 - fm_bpp)) << sh;
    ros_st32(a, (ros_ld32(a) & ~mask) | (v & mask));
}

/* Blend: one channel (mask) of bg weighted 16 - a plus fg weighted a, in
 * 16ths */
static uint32_t blend(uint32_t mask, uint32_t bg, uint32_t ab, uint32_t fg, uint32_t af)
{
    int sh = 0;
    while (!(mask >> sh & 1))
        sh++;
    uint32_t v = ((bg & mask) >> sh) * ab + ((fg & mask) >> sh) * af;
    return (bg & ~mask) | ((v >> 4) << sh & mask);
}

/* The composite for the 32-bpp supremacy and alpha variants (BlendingS
 * blend_putdata_32bpp_nonopaque). *a is the text's weight. The composite's
 * supremacy or alpha goes into the pixel. The result is 0 if the pixel is
 * left alone. */
static int composite32(uint32_t *px, uint32_t *a, uint32_t r2)
{
    if ((int32_t)r2 > 128)
        r2++;
    uint32_t r3 = 4096 - r2 * (16 - *a);
    if (!r3)
        return 0;
    uint32_t q = (*a << 16) / r3;
    q = (q >> 4) + (q >> 3 & 1);
    uint32_t ac = (r3 >> 4) + (r3 >> 3 & 1);
    if ((int32_t)ac >= 128)
        ac--;
    if (fm_blend.mode == 1)
        *px = (*px | 0xFF000000u) - (ac << 24);
    else
        *px = (*px & 0x00FFFFFFu) | ac << 24;
    *a = q;
    return 1;
}

/* a level blended into the pixel (blend_putdata_*) */
static void blend_pixel(int32_t x, int32_t y, uint32_t level)
{
    uint32_t a = level + (level >= 8), w = getaddr(x, y, NULL), px = ros_ld32(w), fg = fm_blend.fg;
    uint32_t k = (uint32_t)x & (fm_ppw - 1);
    switch (fm_blend.kind) {
    case BLEND_32:
        if (fm_blend.mode) {
            uint32_t t = fm_blend.fgalpha * a;
            a = (t >> 8) + (t >> 7 & 1);
            uint32_t r2 = fm_blend.mode == 1 ? px >> 24 : ~(px >> 24);
            if (r2 && !composite32(&px, &a, r2))
                return;
        } else if (a == 16) {
            px = 0;
        }
        px = blend(0xFF, px, 16 - a, fg, a);
        px = blend(0xFF00, px, 16 - a, fg, a);
        px = blend(0xFF0000, px, 16 - a, fg, a);
        break;
    case BLEND_8: {
        uint32_t sh = k * 8, c = ros_ld32(fm_blend.ctable + 4 * (px >> sh & 0xFF));
        c = blend(0x1F, c, 16 - a, fg, a);
        c = blend(0x3E0, c, 16 - a, fg, a);
        c = blend(0x7C00, c, 16 - a, fg, a);
        uint32_t n = ros_ld8(fm_blend.itable + c);
        px = (px & ~(0xFFu << sh)) | n << sh;
        break;
    }
    default: {                                          /* 16 bpp */
        static const uint32_t masks[3][3] = {
            { 0x1F, 0x3E0, 0x7C00 }, { 0x1F, 0x7E0, 0xF800 }, { 0xF, 0xF0, 0xF00 } };
        const uint32_t *m = masks[fm_blend.kind == BLEND_1555 ? 0 : fm_blend.kind == BLEND_565 ? 1 : 2];
        for (int i = 0; i < 3; i++)
            px = blend(m[i] << (16 * k), px, 16 - a, fg, a);
        break;
    }
    }
    ros_st32(w, px);
}

static int in_window(int32_t x, int32_t y)
{
    return x >= fm_vdu.gx0 && x <= fm_vdu.gx1 && y >= fm_vdu.gy0 && y <= fm_vdu.gy1;
}

static uint32_t changed_block(void)
{
    if (!changedbox) {
        uint32_t r[10] = { 0xFFFFFFFFu };
        os_error *e;
        fm_swi(XOS_ChangedBox, r, &e);
        changedbox = e ? 0 : r[1];
    }
    return changedbox;
}

/* changebox: adds the box, clipped to the graphics window, to ChangedBox */
static void changebox(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 < fm_vdu.gx0) x0 = fm_vdu.gx0;
    if (y0 < fm_vdu.gy0) y0 = fm_vdu.gy0;
    if (x1 > fm_vdu.gx1) x1 = fm_vdu.gx1;
    if (y1 > fm_vdu.gy1) y1 = fm_vdu.gy1;
    if (x0 > x1 || y0 > y1)
        return;
    uint32_t b = changed_block() + 4;
    int32_t c[4];
    for (int i = 0; i < 4; i++)
        c[i] = (int32_t)ros_ld32(b + 4 * i);
    if (c[0] > x0) c[0] = x0;
    if (c[1] > y0) c[1] = y0;
    if (c[2] < x1) c[2] = x1;
    if (c[3] < y1) c[3] = y1;
    for (int i = 0; i < 4; i++)
        ros_st32(b + 4 * i, (uint32_t)c[i]);
}

static int changes_on(void)             /* true if ChangedBox is enabled and output is not to a buffer */
{
    uint32_t b = changed_block();
    return b && (ros_ld32(b) & ~(fm_switch_buffer ? 1u : 0u) & 1);
}

/* ---- output to a buffer: rectangles ----------------------------------------------------------- */

#define PATHOBJ_HEADER 40
#define RECTOBJ_SIZE 96

static int counting(void) { return fm_switch_flags & SWF_JUSTCOUNT; }

/* makerectangleobject: makes a filled rectangle object at the buffer's
 * end, in the underline's colour or the rub-out's. It does nothing if one
 * is already under way. */
static os_error *make_rectangle(uint32_t *slot, uint32_t colour, uint32_t *last)
{
    if (*slot)
        return NULL;
    uint32_t b = fm_switch_buffer;
    *slot = b;
    *last = colour;
    if (counting()) {
        fm_switch_buffer = b + RECTOBJ_SIZE;
        return NULL;
    }
    int32_t free = (int32_t)ros_ld32(b + 4) - RECTOBJ_SIZE;
    if (free < 0)
        return fm_err(FE_BUFFOVERFLOW, NULL, NULL);
    ros_st32(b, 2), ros_st32(b + 4, RECTOBJ_SIZE);
    ros_st32(b + 24, colour), ros_st32(b + 28, 0xFFFFFFFFu), ros_st32(b + 32, 0), ros_st32(b + 36, 0);
    uint32_t p = b + PATHOBJ_HEADER;
    ros_st32(p, 2), ros_st32(p + 12, 8), ros_st32(p + 24, 8), ros_st32(p + 36, 8);
    ros_st32(p + 48, 5), ros_st32(p + 52, 0);
    ros_st32(p + 56, 0), ros_st32(p + 60, (uint32_t)free);
    fm_switch_buffer = p + 56;
    return NULL;
}

/* setrectanglesize */
static void rect_size(uint32_t o, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (counting())
        return;
    if (x1 < x0) { int32_t t = x0; x0 = x1, x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1, y1 = t; }
    int32_t v[] = { x0, y0, x1, y1 };
    for (int i = 0; i < 4; i++)
        ros_st32(o + 8 + 4 * i, (uint32_t)v[i]);
    uint32_t p = o + PATHOBJ_HEADER;
    int32_t c[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    for (int i = 0; i < 4; i++)
        ros_st32(p + 12 * i + 4, (uint32_t)c[2 * i]), ros_st32(p + 12 * i + 8, (uint32_t)c[2 * i + 1]);
}

/* makeruboutrectangle: makes a new rub-out rectangle. It is put behind an
 * underline that is under way. */
static os_error *rubout_rect(void)
{
    switch_rubout = 0;
    os_error *e = make_rectangle(&switch_rubout, fm_switch_back, &switch_lastback);
    if (e || counting() || !switch_underline)
        return e;
    uint32_t a = switch_rubout, b = switch_underline;
    switch_underline = a, switch_rubout = b;
    for (uint32_t i = 0; i < RECTOBJ_SIZE; i += 4) {
        uint32_t t = ros_ld32(a + i);
        ros_st32(a + i, ros_ld32(b + i));
        ros_st32(b + i, t);
    }
    return NULL;
}

/* getswitchruboutbox: the box in 1/256 OS units, with x0 as the first x */
static void switch_rub_box(int32_t b[4])
{
    b[0] = (int32_t)((uint32_t)rub[0] << (8 + fm_xeig)), b[2] = (int32_t)((uint32_t)(rub[2] + 1) << (8 + fm_xeig));
    b[1] = (int32_t)((uint32_t)rub[1] << (8 + fm_yeig)), b[3] = (int32_t)((uint32_t)(rub[3] + 1) << (8 + fm_yeig));
    if (fm_plottype & PT_RL) {
        int32_t t = b[0];
        b[0] = b[2], b[2] = t;
    }
}

/* switchnewcolour: a change of background colour splits the rub-out box at
 * the pen. */
static os_error *switch_new_colour(void)
{
    if (fm_switch_back == switch_lastback)
        return NULL;
    int32_t b[4];
    switch_rub_box(b);
    int32_t was = switch_lastrubx1;
    switch_lastrubx1 = fm_divide((int32_t)((uint32_t)fm_xco72 << 8), fm_xscalefactor);
    if (switch_rubout)
        rect_size(switch_rubout, was, b[1], switch_lastrubx1, b[3]);
    os_error *e = rubout_rect();
    if (e)
        return e;
    switch_rub_box(b);
    rect_size(switch_rubout, switch_lastrubx1, b[1], b[2], b[3]);
    return NULL;
}

/* ---- rub-out ------------------------------------------------------------------------------------ */

/* calcrubout: turns the box in millipoints into the pixels whose centres
 * are in it */
static int calc_rubout(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint8_t s;
    int32_t b[4];
    fm_dividex(x0, &b[0], &s);
    if (s >= 2) b[0]++;
    fm_dividey(y0, &b[1], &s);
    if (s >= 2) b[1]++;
    fm_dividex(x1, &b[2], &s);
    if (s < 2) b[2]--;
    fm_dividey(y1, &b[3], &s);
    if (s < 2) b[3]--;
    if (b[2] >= b[0] && b[3] >= b[1]) {
        memcpy(rub, b, sizeof rub);
        ruboutx = b[0];
        return 1;
    }
    ruboutx = BIGNUM;
    setruboutflag = 1;
    return 0;
}

/* getrubaddr: the rub-out restarts at x, on row ruby0 */
static void getrubaddr(int32_t x)
{
    oldrubdata = fm_rubdata;
    uint32_t n;
    ruboutptr = getaddr(x, rub[1], &n);
    uint32_t mask = extendmask(n);
    if (fm_plottype & PT_RL) {
        n = fm_ppw - n;
        mask = ~mask;
    }
    ruboutcount = (uint8_t)n;
    startrubmask = mask;
}

static os_error *setrubout(void)
{
    setruboutflag = 0xFF;
    if (ruboutx == BIGNUM)
        return NULL;
    if (fm_switch_buffer) {                             /* rubout_rectangle: this is not clipped */
        if (rub[0] > rub[2] || rub[1] > rub[3]) {
            ruboutx = BIGNUM;
            return NULL;
        }
        os_error *e = rubout_rect();
        if (e)
            return e;
        int32_t b[4];
        switch_rub_box(b);
        switch_lastrubx1 = b[0];
        rect_size(switch_rubout, b[0], b[1], b[2], b[3]);
    } else {
        int32_t x0 = rub[0], y0 = rub[1], x1 = rub[2], y1 = rub[3];
        if (x0 < fm_vdu.gx0) x0 = fm_vdu.gx0;
        if (y0 < fm_vdu.gy0) y0 = fm_vdu.gy0;
        if (x1 > fm_vdu.gx1) x1 = fm_vdu.gx1;
        if (y1 > fm_vdu.gy1) y1 = fm_vdu.gy1;
        if (x0 > x1 || y0 > y1) {
            ruboutx = BIGNUM;
            return NULL;
        }
        rub[0] = x0, rub[1] = y0, rub[2] = x1, rub[3] = y1;
    }
    int32_t start = rub[0], end = rub[2] + 1;         /* setrubout_common */
    if (fm_plottype & PT_RL)
        start = rub[2] + 1, end = rub[0];
    ruboutx = start;
    getrubaddr(start);
    uint32_t n;
    maxrubptr = getaddr(end, rub[1], &n);
    endrubmask = extendmask(n);
    if (!(fm_plottype & PT_RL))
        endrubmask = ~endrubmask;
    rubouth = rub[3] + 1 - rub[1];
    if (rubouth <= 0)
        ruboutx = BIGNUM;
    return NULL;
}

static os_error *trysetrubout(void)
{
    return setruboutflag ? NULL : setrubout();
}

/* One word of the rub-out, in all its rows. */
static void rub_word(uint32_t w, uint32_t mask)
{
    uint32_t data = fm_rubdata & mask;
    for (int32_t r = 0; r < rubouth; r++, w -= fm_vdu.linelen)
        ros_st32(w, mask == 0xFFFFFFFFu ? data : (ros_ld32(w) & ~mask) | data);
}

/* rubout: fills the box up to this character (x, xsize) and on past it to
 * the glue mid-point. After a change of background colour, it finishes in
 * the old colour first. Then it starts again from the last character's
 * mid-point. */
static os_error *rubout(int32_t x)
{
    if (fm_switch_buffer)
        return switch_new_colour();
    if (fm_rubdata != oldrubdata) {
        uint32_t now = fm_rubdata;
        fm_rubdata = oldrubdata;
        rubout(x);
        fm_rubdata = now;
        x = lastxcoord;                 /* (the end is worked out from this below) */
        if (x < fm_vdu.gx0)
            x = fm_vdu.gx0;
        if (x > fm_vdu.gx1)
            x = fm_vdu.gx1 + 1;
        ruboutx = x;
        getrubaddr(x);
    }
    if (!(fm_plottype & PT_RL)) {
        int32_t end = xsize + x;
        if (end < ul_nextxcoord)
            end = ul_nextxcoord;
        while (ruboutx != BIGNUM && end > ruboutx) {
            uint32_t w = ruboutptr, mask = startrubmask;
            ruboutptr += 4;
            startrubmask = 0xFFFFFFFFu;
            ruboutx += ruboutcount;
            ruboutcount = fm_ppw;
            if ((int32_t)w > (int32_t)maxrubptr)
                continue;
            if (w == maxrubptr)
                mask &= endrubmask;
            rub_word(w, mask);
        }
    } else {
        int32_t end = ul_nextxcoord;
        if (end > x)
            end = x;
        while (end < ruboutx) {
            uint32_t w = ruboutptr, mask = startrubmask;
            ruboutptr -= 4;
            startrubmask = 0xFFFFFFFFu;
            ruboutx -= ruboutcount;
            ruboutcount = fm_ppw;
            if ((int32_t)w < (int32_t)maxrubptr)
                continue;
            if (w == maxrubptr)
                mask &= endrubmask;
            rub_word(w, mask);
        }
    }
    return NULL;
}

/* ruboutrest: fills the rest of the box at the string's end */
static os_error *rubout_rest(void)
{
    if (ruboutx == BIGNUM)
        return NULL;
    os_error *e = fm_set_output();
    if (!e)
        e = trysetrubout();
    if (e || ruboutx == BIGNUM)
        return e;
    lastxcoord = ul_nextxcoord;
    if ((e = rubout(fm_xcoord)))
        return e;
    return rubout(fm_plottype & PT_RL ? rub[0] : rub[2] + 1);
}

/* ---- underline ----------------------------------------------------------------------------------- */

/* Control character 25: the underline's position and thickness, in 256ths
 * of the current font's y size. The position is measured from the pen's
 * glue mid-point. */
static os_error *set_underline(uint32_t *p)
{
    int rl = fm_plottype & PT_RL;
    switch_ulxcoord = rl ? sub(fm_xco72, fm_xglueadd >> 1) : add(fm_xco72, fm_xglueadd >> 1);
    switch_ulycoord = rl ? sub(fm_yco72, fm_yglueadd >> 1) : add(fm_yco72, fm_yglueadd >> 1);
    struct fm_font *f = fm_currentfont < 256 ? fm_fonts[fm_currentfont] : NULL;
    if (!f)
        return fm_err(FE_NOFONT, NULL, NULL);
    int32_t ys = f->ysize;
    int32_t pos = (int32_t)((uint32_t)fm_read_int(p) * (uint32_t)ys * 125) >> 9;
    int32_t th = (int32_t)(fm_read_uint(p) * (uint32_t)ys * 125) >> 9;
    int32_t top = add(switch_ulycoord, pos);
    switch_ultop = top, switch_ulheight = th;
    int32_t t16, b16;
    uint8_t s;
    fm_dividey((int32_t)((uint32_t)top << 4), &t16, &s);
    ul_top = (uint8_t)(t16 & 15);
    ul_ycoord = t16 >> 4;
    fm_dividey(sub((int32_t)((uint32_t)top << 4), (int32_t)((uint32_t)th << 4)), &b16, &s);
    int32_t d = ul_ycoord - (b16 >> 4) - 1;
    uint8_t bot;
    if (d < 0) {
        ul_midheight = 0;
        bot = (uint8_t)(ul_top - (b16 & 15));
        ul_top = bot;
        bot = 0;
    } else {
        ul_midheight = (uint8_t)d;
        bot = (uint8_t)(15 - (b16 & 15));
    }
    ul_bot = bot;
    fm_dividex(switch_ulxcoord, &ul_xcoord, &s);
    switch_underline = 0;
    return NULL;
}

/* One row of the underline: the colour in every pixel inside the window. */
static void ul_row(int32_t x0, int32_t x1, int32_t y, uint32_t colour)
{
    if (y < fm_vdu.gy0 || y > fm_vdu.gy1)
        return;
    for (int32_t x = x0; x < x1; x++)
        put_pixel(x, y, colour);
}

/* switched_underline: the underline is a rectangle from the start point to
 * the glue mid-point. A new one is made when the colour changes. */
static os_error *switched_underline(void)
{
    if (fm_switch_fore != switch_lastfore && switch_underline) {
        switch_underline = 0;
        switch_ulxcoord = switch_ullastx;
    }
    os_error *e = make_rectangle(&switch_underline, fm_switch_fore, &switch_lastfore);
    if (e || counting())
        return e;
    int32_t y1 = fm_divide((int32_t)((uint32_t)switch_ultop << 8), fm_yscalefactor);
    int32_t y0 = fm_divide((int32_t)((uint32_t)sub(switch_ultop, switch_ulheight) << 8), fm_yscalefactor);
    switch_ullastx = switch_nextxcoord;
    int32_t x1 = fm_divide((int32_t)((uint32_t)switch_nextxcoord << 8), fm_xscalefactor);
    int32_t x0 = fm_divide((int32_t)((uint32_t)switch_ulxcoord << 8), fm_xscalefactor);
    rect_size(switch_underline, x0, y0, x1, y1);
    return NULL;
}

static os_error *underline(void)
{
    if (fm_switch_buffer)
        return switched_underline();
    int32_t x0 = ul_xcoord, x1 = ul_nextxcoord;
    ul_xcoord = x1;
    if (x1 < x0) {
        int32_t t = x0;
        x0 = x1, x1 = t;
    }
    if (x0 < fm_vdu.gx0)
        x0 = fm_vdu.gx0;
    if (x1 > fm_vdu.gx1)
        x1 = fm_vdu.gx1 + 1;
    if (x0 >= x1)
        return NULL;
    uint32_t b = changed_block();
    if (b && (ros_ld32(b) & 1))
        changebox(x0, ul_ycoord - ul_midheight - 1, x1 - 1, ul_ycoord);
    int32_t y = ul_ycoord;
    if (ul_top)
        ul_row(x0, x1, y, fm_outputdata[ul_top]);
    y--;
    for (unsigned k = 0; k < ul_midheight; k++, y--)
        ul_row(x0, x1, y, fm_outputdata[15]);
    if (ul_bot)
        ul_row(x0, x1, y, fm_outputdata[ul_bot]);
    return NULL;
}

/* ---- characters ---------------------------------------------------------------------------------- */

/* state for reading the next level of a run-length packed 1-bpp character
 * (getpacked) */
struct packed { const uint8_t *d; uint32_t np, f, repeat, count, state; size_t avail; int depth; };

/* The end of the data is given by the file or the cache. Past it every
 * nibble is 1, which is a short run, so that the loops that read nibbles
 * end. */
static uint32_t nib(struct packed *k)
{
    if (k->np >> 1 >= k->avail) {
        k->np++;
        return 1;
    }
    uint32_t b = k->d[k->np >> 1], r = k->np & 1 ? b >> 4 : b & 15;
    k->np++;
    return r;
}

static uint32_t getpacked(struct packed *k)
{
    uint32_t n = nib(k);
    if (n == 0) {
        uint32_t digits = 1;
        do {
            n = nib(k);
            if (n == 0)
                digits++;
        } while (n == 0);
        uint32_t v = n;
        while (digits--)
            v = (v << 4) + nib(k);
        return v;
    }
    if (n <= k->f)
        return n;
    if (n < 14) {
        uint32_t m = nib(k);
        return ((n - k->f) << 4 | m) + k->f - 15;
    }
    if (k->depth >= 8)              /* (real data nests twice) */
        return 1;
    k->depth++;
    k->repeat = n == 14 ? getpacked(k) : 1;
    uint32_t v = getpacked(k);
    k->depth--;
    return v;
}

/* Paints a bitmap character of w by h pixels. (x0, y0) is its bottom left.
 * The data is at d and is laid out as the character flags say: 4 bpp, or
 * 1 bpp either packed or not. */
static void plot(const uint8_t *d, int32_t x0, int32_t y0, int32_t w, int32_t h, uint8_t flags)
{
    uint8_t row[w > 0 ? w : 1];
    size_t avail = fm_pixel_end && fm_pixel_end > d ? (size_t)(fm_pixel_end - d) : 0;
    struct packed k = { d, 0, flags >> 4, 0, 1, flags & 4 ? 0 : 1, avail, 0 };
    for (int32_t j = 0; j < h; j++) {
        if (!(flags & CHF_1BPP)) {
            for (int32_t i = 0; i < w; i++) {
                uint32_t n = (uint32_t)(j * w + i);
                row[i] = n >> 1 >= avail ? 0 : (uint8_t)(n & 1 ? d[n >> 1] >> 4 : d[n >> 1] & 15);
            }
        } else if (!(flags >> 4)) {
            for (int32_t i = 0; i < w; i++) {
                uint32_t n = (uint32_t)(j * w + i);
                row[i] = n >> 3 < avail && d[n >> 3] >> (n & 7) & 1 ? 15 : 0;
            }
        } else if (k.repeat > 0) {
            k.repeat--;                 /* repeat the row before */
        } else {
            for (int32_t i = 0; i < w; i++) {
                if (--k.count == 0) {
                    k.count = getpacked(&k);
                    k.state ^= 1;
                }
                row[i] = k.state ? 15 : 0;
            }
        }
        for (int32_t i = 0; i < w; i++)
            if (row[i] && in_window(x0 + i, y0 + j)) {
                if (fm_blend.kind)
                    blend_pixel(x0 + i, y0 + j, row[i]);
                else
                    put_pixel(x0 + i, y0 + j, fm_outputdata[row[i]]);
            }
    }
}

/* getbbox: finds the character's box and where its data starts. A bitmap's
 * box is in pixels. An outline's box is found through the render matrix,
 * with a margin. */
static os_error *getbbox(struct fm_font *f, const uint8_t **pp, int32_t b[4])
{
    const uint8_t *p = *pp;
    uint8_t flags = p[0];
    if (!(flags & CHF_OUTLINES)) {
        fm_charflags = flags;
        *pp = fm_read_bbox(p + 1, flags, b);
        return NULL;
    }
    struct fm_font *m;
    os_error *e = fm_font_ptr(f->masterfont, &m);
    if (!e)
        e = fm_bbox_pixels(m, fm_trn ? &fm_trn->render : &f->render, p, b);
    *pp = flags & (CHF_COMPOSITE1 | CHF_COMPOSITE2) ? p + 1 : p + 1 + (flags & CHF_12BIT ? 6 : 4);
    return e;
}

/* ensureheaders: chooses which of the font's data to paint from, and reads
 * its header. It uses 4 bpp when anti-aliased, if the font has it. A
 * transform block has one leaf, which is decided for 1 or 4 bpp as it is
 * first wanted. */
static os_error *pixel_leaf(struct fm_font *f, struct fm_leaf **out, int *bpp1)
{
    os_error *e = NULL;
    if (fm_trn) {
        struct fm_leaf *l = *out = &fm_trn->leaf;
        if (fm_current.acol && l->type >= LEAF_SCAN) {
            if (l->type == LEAF_SCAN)
                e = fm_pixels_header_bpp(f, l, 0);
            if (!e) {
                *bpp1 = 0;
                return NULL;
            }
        }
        if (l->type >= LEAF_SCAN) {
            e = NULL;
            if (l->type == LEAF_SCAN)
                e = fm_pixels_header_bpp(f, l, 1);
            if (!e) {
                *bpp1 = 1;
                return NULL;
            }
        }
        *bpp1 = 0;
        return l->type == LEAF_SCAN ? fm_pixels_header_bpp(f, l, 0) : e;
    }
    if (fm_current.acol && f->leaf4.type >= LEAF_SCAN) {
        if (f->leaf4.type == LEAF_SCAN)
            e = fm_pixels_header(f, &f->leaf4);
        if (!e) {
            *out = &f->leaf4, *bpp1 = 0;
            return NULL;
        }
    }
    if (f->leaf1.type >= LEAF_SCAN) {
        e = NULL;
        if (f->leaf1.type == LEAF_SCAN)
            e = fm_pixels_header(f, &f->leaf1);
        if (!e) {
            *out = &f->leaf1, *bpp1 = 1;
            return NULL;
        }
    }
    e = fm_pixels_header(f, &f->leaf4);
    *out = &f->leaf4, *bpp1 = 0;
    return e;
}

/* paintbuffer_in, paintdraw_common and paintbuffer_out: an outline
 * character as a path object in the buffer, at the pen to the quarter
 * pixel. The object's box comes from its path. A null path makes no
 * object. */
static os_error *paint_to_buffer(struct fm_font *f, const uint8_t *p, int32_t g)
{
    uint32_t obj = fm_switch_buffer;
    struct fm_font *m;
    os_error *e = fm_font_ptr(f->masterfont, &m);
    if (e)
        return e;
    if (!counting()) {
        ros_st32(obj + 24, fm_switch_fore);
        ros_st32(obj + 32, 0);
        ros_st32(obj + 28, 0xFFFFFFFFu);
        ros_st32(obj + 36, m->leaf1.flags & PP_FILLNONZERO ? 0 : 0x40);
        ros_st32(obj, 2);
        ros_st32(obj + PATHOBJ_HEADER, 0);
        ros_st32(obj + PATHOBJ_HEADER + 4, (uint32_t)saved_nextx);  /* (the value R2 held in the original) */
    }
    fm_switch_buffer = obj + PATHOBJ_HEADER;
    int32_t x72 = fm_xco72, y72 = fm_yco72;
    fm_xco72 = (int32_t)((uint32_t)x72 << 2), fm_yco72 = (int32_t)((uint32_t)y72 << 2);
    fm_calcxcoord();
    fm_xco72 = fm_xco72 >> 2, fm_yco72 = fm_yco72 >> 2;
    uint32_t mat = ros_addr(ros_rma_alloc(24));
    ros_st32(mat, 1u << (15 + fm_xeig));
    ros_st32(mat + 4, 0), ros_st32(mat + 8, 0);
    ros_st32(mat + 12, 1u << (15 + fm_yeig));
    ros_st32(mat + 16, (uint32_t)fm_xcoord << 6 << fm_xeig);
    ros_st32(mat + 20, (uint32_t)fm_ycoord << 6 << fm_yeig);
    fm_draw_outline(f, p, (uint32_t)g, mat, &e);
    ros_rma_free(ros_ptr(mat));
    if (e)
        return e;
    uint32_t r1 = fm_switch_buffer;
    int32_t size = (int32_t)(r1 - obj);
    if (size <= PATHOBJ_HEADER) {                       /* nullpath */
        fm_switch_buffer = obj;
        if (!counting()) {
            ros_st32(obj + 4, ros_ld32(r1 + 4) + (uint32_t)size);
            ros_st32(obj, 0);
        }
        return NULL;
    }
    fm_switch_buffer = r1 += 4;
    if (counting())
        return NULL;
    int32_t free = (int32_t)ros_ld32(r1) - 4;
    if (free < 0)
        return fm_err(FE_BUFFOVERFLOW, NULL, NULL);
    ros_st32(r1 + 4, (uint32_t)free);
    ros_st32(r1, 0);
    ros_st32(obj + 4, r1 - obj);
    int32_t b[4] = { BIGNUM, BIGNUM, -BIGNUM, -BIGNUM };
    for (uint32_t q = obj + PATHOBJ_HEADER;;) {
        uint32_t t = ros_ld32(q);
        q += 4;
        int n = t == 2 || t == 8 ? 1 : t == 6 ? 3 : t == 5 ? 0 : -1;
        if (n < 0)
            break;
        for (; n; n--, q += 8) {
            int32_t x = (int32_t)ros_ld32(q), y = (int32_t)ros_ld32(q + 4);
            if (x < b[0]) b[0] = x;
            if (x > b[2]) b[2] = x;
            if (y < b[1]) b[1] = y;
            if (y > b[3]) b[3] = y;
        }
    }
    for (int i = 0; i < 4; i++)
        ros_st32(obj + 8 + 4 * i, (uint32_t)b[i]);
    return NULL;
}

/* paintdraw: an outline character through Draw, in the font's colour, with
 * the pen at its whole pixel */
static os_error *paint_draw(struct fm_font *f, const uint8_t *p, int32_t g)
{
    if (fm_switch_buffer)
        return paint_to_buffer(f, p, g);
    fm_restore_output();
    uint32_t buf = ros_addr(ros_rma_alloc(64)), mat = buf + 40;
    uint32_t r[10] = { 0x80, buf + 8 };
    os_error *e;
    fm_swi(XOS_SetColour, r, &e);
    if (e) {
        ros_rma_free(ros_ptr(buf));
        return e;
    }
    uint32_t saved0 = r[0], saved1 = r[1];
    uint32_t colour = fm_outputdata[15];
    if (fm_bpp != 32)
        colour >>= 32 - fm_bpp;
    uint32_t c[10] = { 0, colour };
    fm_swi(XOS_SetColour, c, &e);
    ros_st32(mat, 1u << (15 + fm_xeig));
    ros_st32(mat + 4, 0), ros_st32(mat + 8, 0);
    ros_st32(mat + 12, 1u << (15 + fm_yeig));
    ros_st32(mat + 16, (uint32_t)fm_oldxcoord << 8 << fm_xeig);
    ros_st32(mat + 20, (uint32_t)fm_oldycoord << 8 << fm_yeig);
    fm_draw_outline(f, p, (uint32_t)g, mat, &e);
    uint32_t back[10] = { saved0, saved1 };
    os_error *e2;
    fm_swi(XOS_SetColour, back, &e2);
    if (e2)
        e = e2;
    ros_rma_free(ros_ptr(buf));
    return e;
}

/* addcharwidth: works out the pen at this character (after the glue), the
 * pen for the next character, and the glue mid-point after it. The
 * mid-point is used for rub-out and underline. */
static os_error *add_char_width(int32_t g, uint32_t next)
{
    int32_t x = fm_xco72, y = fm_yco72, w, h;
    os_error *e;
    int rl = fm_plottype & PT_RL;
    if (!rl) {
        x = add(x, fm_xglueadd), y = add(y, fm_yglueadd);
        fm_xco72 = x, fm_yco72 = y;
        if ((e = fm_char_width(g, next, &w, &h)))
            return e;
        w = add(x, w), h = add(y, h);
    } else {
        x = sub(x, fm_xglueadd), y = sub(y, fm_yglueadd);
        if ((e = fm_char_width(g, next, &w, &h)))
            return e;
        w = sub(x, w), h = sub(y, h);
        fm_xco72 = w, fm_yco72 = h;
    }
    saved_nextx = w, saved_nexty = h;
    switch_nextxcoord = rl ? sub(w, fm_xglueadd >> 1) : add(w, fm_xglueadd >> 1);
    uint8_t s;
    fm_dividex(switch_nextxcoord, &ul_nextxcoord, &s);
    return NULL;
}

static os_error *paint_char(int32_t g, uint32_t next)
{
    os_error *e = fm_set_output();
    if (!e)
        e = trysetrubout();
    if (e)
        return e;
    lastxcoord = ul_nextxcoord;
    if ((e = add_char_width(g, next)))
        return e;
    fm_calcxcoord();
    struct fm_font *f;
    struct fm_leaf *l;
    const uint8_t *p = NULL;
    if ((e = fm_font_ptr(fm_currentfont, &f)))
        return e;
    if (fm_paintmatrix != fm_oldpaintmatrix) {         /* SetPixelsPtr: GetTransform */
        fm_oldpaintmatrix = fm_paintmatrix;
        if ((e = fm_get_transform(f)))
            return e;
    }
    int bpp1;
    if ((e = pixel_leaf(f, &l, &bpp1)) || (e = fm_pixel_char(f, l, bpp1, (uint32_t)g, &p)))
        return e;
    int32_t x = fm_xcoord, y = fm_ycoord;
    xsize = 0;
    if (p) {
        int32_t b[4];
        if ((e = getbbox(f, &p, b)))
            return e;
        x += b[0], y += b[1];
        xsize = b[2], rowcount = b[3];
        if (!(b[2] | b[3]))
            p = NULL;
        else if (changes_on())
            changebox(x, y, x + xsize - 1, y + rowcount - 1);
    }
    if (ruboutx != BIGNUM)
        e = rubout(x);
    if (!e && (ul_top | ul_midheight | ul_bot))
        e = underline();
    if (!e && p) {
        if (fm_charflags & CHF_OUTLINES)
            e = paint_draw(f, p, g);
        else if (fm_switch_flags & SWF_ENABLED)
            e = fm_switch_flags & SWF_NOBITMAPS ? fm_err(FE_NOBITMAPS2, NULL, NULL) : NULL;
        else if (xsize > 0 && rowcount > 0)
            plot(p, x, y, xsize, rowcount, fm_charflags);
    }
    if (!e)
        fm_xco72 = saved_nextx, fm_yco72 = saved_nexty;
    return e;
}

/* ---- Font_Paint --------------------------------------------------------------------------------- */

/* calcjustify: the old way of justifying. The space that the string leaves
 * before the justify point (the graphics cursor) is shared among its
 * spaces. */
static os_error *calc_justify(uint32_t str)
{
    fm_xletteradd = fm_yletteradd = 0;
    if (!(fm_plottype & PT_JUSTIFY)) {
        fm_xspaceadd = fm_yspaceadd = 0;
        return NULL;
    }
    int32_t lim = (int32_t)endaddr < 0 ? BIGNUM : endaddr ? (int32_t)(endaddr - str) : 0;
    struct fm_pars r;
    os_error *e = fm_scan_string(str, BIGNUM, BIGNUM, ' ', lim, 0, 0, &r);
    if (e)
        return e;
    int32_t jx = sub(scalexco(justifyx), fm_xco72), w = r.x;
    if (jx < 0) jx = -jx;
    if (w < 0) w = -w;
    int32_t gap = sub(jx, w);
    fm_xspaceadd = r.n ? fm_divide(gap, r.n) : 0;
    fm_yspaceadd = 0;
    return NULL;
}

static os_error *paint_chars(const uint32_t *R)
{
    uint32_t flags = R[2], reserved = PT_RESERVED;
    if (flags & PT_COORDBLK)
        reserved |= PT_OSUNITS | PT_JUSTIFY;
    if (flags & reserved)
        return fm_err(FE_RESERVED, NULL, NULL);
    if ((flags & PT_FONT) && R[0])
        fm_currentfont = R[0] & 0xFF;
    uint32_t str = R[1], p = str;
    fm_plottype = flags;
    fm_check_blend();
    endaddr = flags & PT_LENGTH ? str + R[7] : 0xFFFFFFFFu;
    if (!(flags & PT_OSUNITS)) {
        fm_xco72 = (int32_t)R[3], fm_yco72 = (int32_t)R[4];
        fm_calcxcoord();
    } else {
        fm_xco72 = scalexco((int32_t)R[3] >> fm_xeig);
        fm_yco72 = scaleyco((int32_t)R[4] >> fm_yeig);
    }
    setruboutflag = 0;
    ul_top = ul_midheight = ul_bot = 0;
    switch_underline = switch_rubout = 0;
    fm_inscanstring = 0;
    int changed = 0;
    if (flags & PT_COORDBLK) {
        uint32_t c = R[5];
        fm_xspaceadd = (int32_t)ros_ld32(c), fm_yspaceadd = (int32_t)ros_ld32(c + 4);
        fm_xletteradd = (int32_t)ros_ld32(c + 8), fm_yletteradd = (int32_t)ros_ld32(c + 12);
        if (flags & PT_RUBOUT) {
            changed = calc_rubout((int32_t)ros_ld32(c + 16), (int32_t)ros_ld32(c + 20),
                                  (int32_t)ros_ld32(c + 24), (int32_t)ros_ld32(c + 28));
        } else {
            ruboutx = BIGNUM;
            setruboutflag = 1;
        }
    } else {
        const int32_t *cs = fm_vdu.cursors;     /* OlderCs, OldCs, GCsI */
        int k = 4;
        if (flags & PT_JUSTIFY) {
            justifyx = cs[4], justifyy = cs[5];
            k = 2;
        }
        if (!(flags & PT_RUBOUT)) {
            ruboutx = BIGNUM;
            setruboutflag = 1;
        } else {
            rub[2] = cs[k], rub[3] = cs[k + 1];
            rub[0] = cs[k - 2], rub[1] = cs[k - 1];
            ruboutx = rub[0];
            changed = 1;
        }
    }
    if (changed && changes_on())
        changebox(rub[0], rub[1], rub[2], rub[3]);
    os_error *e = NULL;
    if (!(flags & PT_COORDBLK) && (e = calc_justify(str)))
        return e;
    fm_set_paint_matrix(flags & PT_MATRIX ? ros_ptr(R[6]) : NULL);
    fm_oldpaintmatrix = FM_NOMATRIX;
    fm_met.valid = 0;
    fm_xglueadd = fm_yglueadd = 0;
    struct fm_font *f;
    for (;;) {
        if (p >= endaddr)
            break;
        uint32_t c;
        int32_t g;
        if ((e = fm_next_char(&p, &c)))
            return e;
        fm_extcode = c;
        if (c >= 32) {
            if ((e = fm_font_ptr(fm_currentfont, &f)) || (e = fm_map_char(f, (int32_t)c, &g)))
                return e;
            if (g != -1 && (e = paint_char(g, p)))
                return e;
            continue;
        }
        switch (c) {
        case 17: {                                          /* colour */
            uint32_t v = fm_read_uint(&p);
            if (v < 0x80)
                fm_current.fcol = (uint8_t)v;
            else
                fm_current.bcol = (uint8_t)(v - 0x80);
            fm_outputvalid = 0;
            fm_current.rgb_a = 0x80;
            continue;
        }
        case 18:                                            /* colours and offset */
            fm_current.bcol = (uint8_t)(fm_read_uint(&p) & 0x7F);
            fm_current.fcol = (uint8_t)fm_read_uint(&p);
            fm_current.acol = (uint8_t)fm_read_int(&p);
            fm_outputvalid = 0;
            fm_current.rgb_a = 0x80;
            continue;
        case 19: {                                          /* RGB colours */
            uint32_t b = fm_read_rgb(&p), fg = fm_read_rgb(&p), mx = fm_read_uint(&p);
            fm_current.rgb_b = b, fm_current.rgb_f = fg, fm_current.rgb_a = mx;
            fm_switch_back = b, fm_switch_fore = fg;
            fm_outputvalid = 0;
            fm_restore_output();
            uint32_t r[10] = { 0, b, fg, mx };
            fm_swi(XColourTrans_SetFontColours, r, &e);
            if (!e) {
                fm_current.bcol = (uint8_t)r[1], fm_current.fcol = (uint8_t)r[2];
                fm_current.acol = (uint8_t)r[3];
            }
            fm_current.rgb_b = b, fm_current.rgb_f = fg, fm_current.rgb_a = mx;
            fm_switch_back = b, fm_switch_fore = fg;
            if (e)
                return e;
            continue;
        }
        case 26:                                            /* font */
            fm_currentfont = fm_read_uint(&p) & 0xFF;
            fm_tfuture.font = fm_currentfont;
            fm_futurechanged = 1;
            fm_oldpaintmatrix = FM_NOMATRIX;
            fm_met.valid = 0;
            continue;
        case 21:                                            /* comment */
            while (fm_read_uint(&p) >= 32)
                ;
            continue;
        case 25:
            if ((e = set_underline(&p)))
                return e;
            continue;
        case 9:
            fm_xco72 = add(fm_xco72, fm_read_int3(&p));
            continue;
        case 11:
            fm_yco72 = add(fm_yco72, fm_read_int3(&p));
            continue;
        case 27:                                            /* a matrix, no translation */
            p = ((p + 3) & ~3u) + 16;
            for (int i = 0; i < 4; i++)
                fm_paintmatrixbuffer[i] = (int32_t)ros_ld32(p - 16 + 4 * i);
            fm_paintmatrixbuffer[4] = fm_paintmatrixbuffer[5] = 0;
            if (fm_oldpaintmatrix == (const uint8_t *)fm_paintmatrixbuffer)
                fm_oldpaintmatrix = FM_NOMATRIX;
            fm_set_paint_matrix((const uint8_t *)fm_paintmatrixbuffer);
            fm_met.valid = 0;
            continue;
        case 28:                                            /* a matrix */
            p = ((p + 3) & ~3u) + 24;
            fm_set_paint_matrix(ros_ptr(p - 24));
            fm_met.valid = 0;
            continue;
        case 0: case 10: case 13:
            break;
        default:
            return fm_err(FE_BADCTRLCHAR, NULL, NULL);
        }
        break;
    }
    fm_calcxcoord();
    return rubout_rest();
}

/* The string of VDU 25,&D0 to &D7. It is painted at the plot's point, which
 * is NewPt as setmodedata read it when the plot came, so the origin is
 * included. The point is in millipoints. Bits 0 and 1 of the plot code are
 * the justify and rub-out flags. */
os_error *fm_paint_vdu(uint32_t str, uint32_t plottype)
{
    uint32_t R[8] = { 0, str, plottype, (uint32_t)scalexco(fm_vdu.cursors[6]),
                      (uint32_t)scaleyco(fm_vdu.cursors[7]) };
    uint32_t r[10] = { 0 };
    os_error *e, *e2;
    fm_swi(XOS_RemoveCursors, r, &e);
    if (!e)
        e = paint_chars(R);
    fm_restore_output();
    fm_swi(XOS_RestoreCursors, r, &e2);
    return e ? e : e2;
}

int (*fm_paint_blend_hook)(void);

void fm_thunk_Paint(struct ros_cpu *s)
{
    uint32_t r[10] = { 0 }, R[8];
    os_error *e, *e2;
    if (fm_printerflag) {                       /* TryPrinterDriver: the printer driver paints */
        uint32_t p[10];
        memcpy(p, s->r, sizeof p);
        p[8] = 0x06;                            /* Font_Paint's offset in the SWI chunk */
        fm_swi(XPDriver_FontSWI, p, &e);
        if (e) {
            ros_swi_fail(s, e);
            return;
        }
        memcpy(s->r, p, 8 * sizeof p[0]);
        s->v = 0;
        return;
    }
    memcpy(R, s->r, sizeof R);
    if (!(R[2] & 0x800) && fm_paint_blend_hook && fm_paint_blend_hook())
        R[2] |= 0x800;                          /* blending is forced (modules/smooth) */
    fm_swi(XOS_RemoveCursors, r, &e);
    if (!e)
        e = fm_mode_vars();
    if (!e)
        e = paint_chars(R);
    fm_restore_output();
    fm_swi(XOS_RestoreCursors, r, &e2);
    if (!e)
        e = e2;
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

/* ---- Font_Caret ------------------------------------------------------------------------------------ */

/* One row of the caret: the pattern's bits, with bit 0 at x, are EORed in
 * inside the window. */
static void caret_row(uint32_t bits, int32_t x, int32_t y, uint32_t colour)
{
    if (y >= fm_vdu.gy0 && y <= fm_vdu.gy1)
        for (int k = 0; bits >> k; k++)
            if (bits >> k & 1 && x + k >= fm_vdu.gx0 && x + k <= fm_vdu.gx1) {
                uint32_t a = getaddr(x + k, y, NULL);
                uint32_t sh = ((uint32_t)(x + k) & (fm_ppw - 1)) * fm_bpp;
                ros_st32(a, ros_ld32(a) ^ (fm_bpp == 32 ? colour : (colour >> (32 - fm_bpp)) << sh));
            }
}

static void paint_caret(const uint32_t *R)
{
    int32_t rows = (int32_t)R[1] >> fm_yeig;
    int dbl = R[2] & 0x20;
    uint32_t small = dbl ? 0x18 : 0x10, mid = dbl ? 0x3C : 0x28, big = dbl ? 0xE7 : 0xC6;
    int32_t cross = 1;
    if (dbl && !(cross = (int32_t)((2u << fm_xeig) >> fm_yeig)))
        cross = 1;
    int32_t x, y;
    if (!(R[2] & 0x10)) {
        fm_xco72 = (int32_t)R[3], fm_yco72 = (int32_t)R[4];
        fm_calcxcoord();
        x = fm_xcoord, y = fm_ycoord;
    } else {
        x = (int32_t)R[3] >> fm_xeig, y = (int32_t)R[4] >> fm_yeig;
    }
    x -= 4;
    uint32_t colour = R[0] << (32 - fm_bpp);
    int32_t cut = (int32_t)((16u << fm_xeig) >> fm_yeig);
    uint32_t seq[5];
    int32_t count[5];
    int n = 0;
    if (rows >= cut + (cut >> 1)) {
        seq[n] = big, count[n++] = cross;
        seq[n] = mid, count[n++] = cross;
        seq[n] = small, count[n++] = rows - 4 * cross;
        seq[n] = mid, count[n++] = cross;
        seq[n] = big, count[n++] = cross;
    } else if (rows >= cut) {
        seq[n] = mid, count[n++] = cross;
        seq[n] = small, count[n++] = rows - 2 * cross;
        seq[n] = mid, count[n++] = cross;
    } else {
        seq[n] = small, count[n++] = rows;
    }
    for (int i = 0; i < n; i++)
        for (int32_t k = 0; k < count[i]; k++)
            caret_row(seq[i], x, y++, colour);
}

void fm_thunk_Caret(struct ros_cpu *s)
{
    uint32_t r[10] = { 0 };
    os_error *e, *e2;
    fm_swi(XOS_RemoveCursors, r, &e);
    if (!e)
        e = fm_mode_vars();
    if (!e)
        paint_caret(s->r);
    fm_swi(XOS_RestoreCursors, r, &e2);
    if (!e)
        e = e2;
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

/* ---- Font_SwitchOutputToBuffer ----------------------------------------------------------------- */

void fm_thunk_SwitchOutputToBuffer(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1];
    int off = r1 == 0 || r1 == 0xFFFFFFFFu;
    if (r0 & ~(off ? 0u : (uint32_t)(SWF_JUSTCOUNT | SWF_SCAFFOLD | SWF_NOBITMAPS))) {
        ros_swi_fail(s, fm_err(FE_RESERVED, NULL, NULL));
        return;
    }
    uint32_t oldf = fm_switch_flags & ~SWF_ENABLED, oldb = fm_switch_buffer;
    if (!off)
        r0 |= SWF_ENABLED;
    if (r1 != 0xFFFFFFFFu)
        fm_switch_flags = r0, fm_switch_buffer = r1;
    s->r[0] = oldf, s->r[1] = oldb;
    s->v = 0;
}
