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
 * (Sources/Kernel: s.vdu.vduwrch, s.vdu.vducursoft, s.vdu.vdugrafl).
 */
/* text.c -- text on the screen: colours, characters, clearing, scrolling,
 * the cursor.
 *
 * This follows the kernel's Kernel/s/vdu/vduwrch, vducursoft and
 * vdugrafl.
 *
 *   - Colours. COLOUR's number is masked by NColour and 63. Below 256
 *     colours it is the pixel. At 256 colours it goes with its tint
 *     through the kernel's bit shuffle, B3 G3 G2 R3 B2 R2 T1 T0. In true
 *     colour each two-bit component c with the tint t becomes
 *     c<<6|t<<4|c<<2|t, packed as ColourTrans_ReturnColourNumber packs it.
 *     That is done here and not through ColourTrans, which only packs.
 *   - A character is its eight font bytes, with bit 7 the leftmost pixel.
 *     It is painted opaque in the text colours over the whole cell at the
 *     cursor. Each row is painted twice in double-vertical modes. In gap
 *     modes other than the BBC's (3, 6) the two gap rows are painted in
 *     the background. Pixels are little-endian, with the leftmost lowest.
 *     In double-pixel modes a character pixel is two of the screen's,
 *     BytesPerChar bits.
 *   - Clearing fills with the background. In BBC gap modes each text row's
 *     rows 8 and 9 are colour 2. CLS with no text window fills all of
 *     ScreenSize.
 *   - Scrolling copies the text window if one was ever set, or else the
 *     screen. It then clears the new row.
 *   - The cursor is the cell's rows [start, end) exclusive-ORed with
 *     CursorFill. CursorFill is &77.. at 4 bpp, &55.. in BBC gap modes,
 *     &07.. in 256-colour teletext, and otherwise &FF.. . The cursor is
 *     drawn in both of teletext's banks. It is taken off around every VDU
 *     operation and put back after, and it flashes on the VSync: 16
 *     VSyncs a phase when slow and 8 when fast.
 *
 * In teletext (CF_TELETEXT) characters, clearing and scrolling go through
 * the page's map instead (ttx.c), and CLS clears both banks.
 */
#include <string.h>

#include "rosgd/arena.h"
#include "rosgd/vdu.h"
#include "vduws.h"

/* ---- colours -------------------------------------------------------------- */

static uint32_t ror32(uint32_t v, uint32_t n)
{
    n &= 31;
    return n ? v >> n | v << (32 - n) : v;
}

/* SetColours' spreading. At 16 bpp and under, the colour is rotated to the
 * top of the word by BitsPerPix and by BytesPerChar and spread down it.
 * Above 16 bpp the colour is used as it is. Nothing is masked. A colour
 * number with bits above its pixel's, such as teletext's 64K transparency
 * masks or OS_SetColour's, leaves them in the lower lanes of the word, as
 * the kernel's does. Each pixel is painted from its own lane (the kernel's
 * TextExpand table words, and CLS's whole words). Spreading a spread colour
 * again changes nothing, unless it had such bits. */
static uint32_t replicate(uint32_t v)
{
    if (vdu.bpp > 16)
        return v;
    v = ror32(v, vdu.bpp) | ror32(v, vdu.bpc);
    for (uint32_t s = vdu.bpc; s < 32; s *= 2)
        v |= v >> s;
    return v;
}

/* SetColours. TextFgColour and TextBgColour are spread through their words
 * in place, as the painters use them. This is what OS_SetColour reads
 * back, a save area keeps and VDU 23,17,5 swaps. A colour set since then
 * (by OS_SetColour, COLOUR or a tint) waits as a number, with TEUpdate set,
 * until the next thing that paints spreads both again. */
void vdu_set_colours(void)
{
    vdu.text_fg = replicate(vdu.text_fg);
    vdu.text_bg = replicate(vdu.text_bg);
    vdu.cursor_flags &= ~CF_TEUPDATE;
}

/* Makes a true-colour pixel from 8-bit components, as ColourTrans packs
 * it. */
static uint32_t pack(uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t f = vdu.mv[MV_FLAGS], rgb = f & MF_RGB;
    if (vdu.mv[MV_LOG2BPP] == 5)
        return rgb ? r << 16 | g << 8 | b : b << 16 | g << 8 | r;
    if (vdu.mv[MV_NCOLOUR] == 0xFFF)
        return rgb ? (r >> 4) << 8 | (g >> 4) << 4 | b >> 4 : (b >> 4) << 8 | (g >> 4) << 4 | r >> 4;
    if (f & MF_FULLPALETTE)             /* 5:6:5 */
        return rgb ? (r >> 3) << 11 | (g >> 2) << 5 | b >> 3 : (b >> 3) << 11 | (g >> 2) << 5 | r >> 3;
    return rgb ? (r >> 3) << 10 | (g >> 3) << 5 | b >> 3 : (b >> 3) << 10 | (g >> 3) << 5 | r >> 3;
}

/* ConvertGCOLToColourNumber: g = t t b b g g r r */
uint32_t vdu_gcol_to_colour(uint32_t g)
{
    uint32_t nc = vdu.mv[MV_NCOLOUR];
    if (nc == 63 || nc == 255)
        return (g >> 6 & 3) | (g & 1) << 2 | (g >> 4 & 1) << 3 | (g >> 1 & 1) << 4 |
               (g >> 2 & 1) << 5 | (g >> 3 & 1) << 6 | (g >> 5 & 1) << 7;
    uint32_t t = g >> 6 & 3;
#define COMPONENT(c) ((c) << 6 | t << 4 | (c) << 2 | t)
    return pack(COMPONENT(g & 3), COMPONENT(g >> 2 & 3), COMPONENT(g >> 4 & 3));
#undef COMPONENT
}

void vdu_compile_fg(void)
{
    vdu.text_fg = vdu.mv[MV_NCOLOUR] >= 63 ? vdu_gcol_to_colour(vdu.tfore | (vdu.tftint & 0xC0))
                                           : vdu.tfore;
}

void vdu_compile_bg(void)
{
    vdu.text_bg = vdu.mv[MV_NCOLOUR] >= 63 ? vdu_gcol_to_colour(vdu.tback | (vdu.tbtint & 0xC0))
                                           : vdu.tback;
}

void vdu_default_colours(void)
{
    uint32_t nc = vdu.mv[MV_NCOLOUR], flags = vdu.mv[MV_FLAGS];
    vdu.gplfmd = vdu.gplbmd = 0;
    vdu.gfcol = nc & 0xF0 ? nc : nc & 7;
    vdu.gbcol = 0;
    vdu.tftint = 0xFF, vdu.tbtint = 0, vdu.gftint = 0xFF, vdu.gbtint = 0;
    vdu_set_colour();

    uint32_t fore = nc, back = 0;
    if (fore == 63)
        fore = 255;
    if (fore == 15 && !(flags & MF_GREYSCALE))
        fore = 7;
    if (fore > 255) {                   /* true colour is white, with the alpha bits */
        uint32_t alpha;
        if (nc == 0xFFFFFFFFu)
            fore = 0x00FFFFFF, alpha = 0xFF000000u;
        else if (nc == 0xFFF)
            fore = 0xFFF, alpha = 0xF000;
        else if (flags & MF_FULLPALETTE)
            fore = 0xFFFF, alpha = 0;
        else
            fore = 0x7FFF, alpha = 0x8000;
        if (flags & MF_ALPHA)
            fore |= alpha, back = alpha;
    }
    vdu.tfore = vdu.text_fg = fore;
    vdu.tback = vdu.text_bg = back;
    vdu_set_colours();
}

/* ---- painting ---------------------------------------------------------------- */

static uint8_t *cell(int32_t col, int32_t row)
{
    return vdu.screen + vdu.text_offset + (uint32_t)row * vdu.row_length +
           (uint32_t)col * vdu.char_width;
}

/* Paints one row of a character cell from the font byte's bits, with bit
 * 7 leftmost. Below 8 bpc the row is the kernel's TextExpand entry for the
 * byte. That is bpc bytes at bits * bpc in a table made a word at a time
 * from the colour words. So its byte i is from lane (bits * bpc + i) & 3
 * of them. The lane follows the font byte at 1 and 2 bpc, and is the
 * byte's own at 4 bpc. */
static void put_row(uint8_t *p, uint32_t bits)
{
    uint32_t fg = vdu.text_fg, bg = vdu.text_bg, bpc = vdu.bpc;
    if (bpc < 8) {
        uint32_t mask = 0, one = (1u << bpc) - 1;
        for (uint32_t x = 0; x < 8; x++)
            if (bits >> (7 - x) & 1)
                mask |= one << (x * bpc);
        for (uint32_t i = 0; i < bpc; i++) {        /* bpc bytes, being 8 pixels of bpc bits */
            uint32_t lane = 8 * ((bits * bpc + i) & 3), m = mask >> (8 * i);
            p[i] = (uint8_t)((m & fg >> lane) | (~m & bg >> lane));
        }
        return;
    }
    uint32_t n = bpc / 8;
    for (uint32_t x = 0; x < 8; x++) {
        uint32_t v = bits >> (7 - x) & 1 ? fg : bg;
        v >>= 8 * (x * n & 3);                      /* the pixel's lane of the word */
        memcpy(p + x * n, &v, n);                   /* the low bytes, as it is little-endian */
    }
}

void vdu_paint_char(const uint8_t glyph[8])
{
    if (VDU_CLIPPING)          /* ClipCursorCell */
        vdu_cbox_text(vdu.cx, vdu.cy, vdu.cx, vdu.cy);
    if (!vdu.screen_ok)
        return;
    if (vdu.cursor_flags & CF_TEUPDATE)
        vdu_set_colours();
    uint8_t *p = cell(vdu.cx, vdu.cy);
    uint32_t ll = vdu.mv[MV_LINELENGTH], f = vdu.mv[MV_FLAGS];
    int dbl = (f & MF_DOUBLEVERTICAL) != 0;
    for (uint32_t r = 0; r < 8; r++) {
        put_row(p + (dbl ? 2 * r : r) * ll, glyph[r]);
        if (dbl)
            put_row(p + (2 * r + 1) * ll, glyph[r]);
    }
    if ((f & MF_GAP) && !(f & MF_BBCGAP))       /* at 2 bpc both bytes are the table's
                                                   first, the background's low byte */
        for (uint32_t r = vdu.tchar_y; r < vdu.row_mult; r++) {
            if (vdu.bpc == 2)
                p[r * ll] = p[r * ll + 1] = (uint8_t)vdu.text_bg;
            else
                put_row(p + r * ll, 0);
        }
}

/* Fills n bytes of one pixel row at p with a replicated colour word, as
 * ClearThisBox and FastCLS store it. Whole words are stored where they are
 * aligned, and the bytes either side of them are each the word's low
 * byte. */
static void fill(uint8_t *p, uint32_t n, uint32_t word)
{
    uint8_t *end = p + n;
    if (word == (word & 0xFF) * 0x01010101u) {
        memset(p, (int)(word & 0xFF), n);
        return;
    }
    while (p < end && ((uintptr_t)p & 3))
        *p++ = (uint8_t)word;
    for (; end - p >= 4; p += 4)
        memcpy(p, &word, 4);
    while (p < end)
        *p++ = (uint8_t)word;
}

/* ClearThisBox. This clears w bytes of each pixel row from p, for the
 * given number of text rows. In BBC gap modes each text row's rows 8 and 9
 * are colour 2. */
static void clear_this_box(uint8_t *p, uint32_t w, int32_t rows)
{
    if (vdu.cursor_flags & CF_TEUPDATE)
        vdu_set_colours();
    uint32_t ll = vdu.mv[MV_LINELENGTH];
    int bbcgap = (vdu.mv[MV_FLAGS] & MF_BBCGAP) != 0;
    for (uint32_t y = 0; y < (uint32_t)rows * vdu.row_mult; y++, p += ll)
        fill(p, w, bbcgap && y % vdu.row_mult >= 8 ? 0xAAAAAAAAu : vdu.text_bg);
}

/* ClearBox. This clears text columns l to r and rows t to b, inclusive. */
void vdu_clear_box(int32_t l, int32_t t, int32_t r, int32_t b)
{
    if (VDU_CLIPPING)
        vdu_cbox_text(l, b, r, t);
    if (vdu.cursor_flags & CF_TELETEXT) {
        vdu_ttx_clear_box(l, t, r, b);
        return;
    }
    if (!vdu.screen_ok || l > r || t > b)
        return;
    clear_this_box(cell(l, t), (uint32_t)(r - l + 1) * vdu.char_width, b - t + 1);
}

/* VDU 12, in VDU 4. */
void vdu_cls(void)
{
    vdu.page_lines = 0;
    vdu.cx = vdu.twl, vdu.cy = vdu.twt;
    vdu.cursor_flags &= ~CF_C81;
    if (!(vdu.status & VS_WINDOWING) && VDU_CLIPPING)
        vdu_cbox_full();
    if ((vdu.status & VS_WINDOWING) || (vdu.mv[MV_FLAGS] & MF_BBCGAP)) {
        vdu_clear_box(vdu.twl, vdu.twt, vdu.twr, vdu.twb);
        return;
    }
    if (vdu.cursor_flags & CF_TEUPDATE)
        vdu_set_colours();
    if (vdu.cursor_flags & CF_TELETEXT)
        vdu_ttx_cls();                  /* the map, and black (transparent) */
    if (!vdu.screen_ok)
        return;
    fill(vdu.screen, vdu.mv[MV_SCREENSIZE], vdu.text_bg);
}

/* The area that a scroll moves. It is the text window, or the screen if
 * none is set. */
static void scroll_area(int32_t *l, int32_t *t, int32_t *r, int32_t *b)
{
    if (vdu.status & VS_WINDOWING)
        *l = vdu.twl, *t = vdu.twt, *r = vdu.twr, *b = vdu.twb;
    else
        *l = 0, *t = 0, *r = (int32_t)vdu.mv[MV_SCRRCOL], *b = (int32_t)vdu.mv[MV_SCRBROW];
}

static void move_rows(int32_t l, int32_t r, int32_t to, int32_t from, int32_t rows)
{
    uint32_t ll = vdu.mv[MV_LINELENGTH];
    if (!(vdu.status & VS_WINDOWING)) {         /* the whole screen, in whole lines */
        memmove(vdu.screen + (uint32_t)to * vdu.row_length,
                vdu.screen + (uint32_t)from * vdu.row_length, (uint32_t)rows * vdu.row_length);
        return;
    }
    uint32_t width = (uint32_t)(r - l + 1) * vdu.char_width, lines = (uint32_t)rows * vdu.row_mult;
    uint8_t *dst = cell(l, to), *src = cell(l, from);
    if (to < from)
        for (uint32_t y = 0; y < lines; y++)
            memmove(dst + y * ll, src + y * ll, width);
    else
        for (uint32_t y = lines; y-- > 0;)
            memmove(dst + y * ll, src + y * ll, width);
}

/* Scrolls a box's rows up or down one. The row left behind is cleared. */
static void scroll_rows(int up, int32_t l, int32_t t, int32_t r, int32_t b)
{
    if (vdu.screen_ok && b > t) {
        if (up)
            move_rows(l, r, t, t + 1, b - t);
        else
            move_rows(l, r, t + 1, t, b - t);
    }
    vdu_clear_box(l, up ? b : t, r, up ? b : t);
}

/* The text's own scrolls, which are LF's and VT's. With ChangedBox on, the
 * kernel's go the way VDU 23,7's do (SpecialLF and SpecialVT), adding the
 * window. */
static void text_scroll(int up)
{
    int32_t l, t, r, b;
    if (VDU_CLIPPING)
        vdu_cbox_text(vdu.twl, vdu.twb, vdu.twr, vdu.twt);
    if (vdu.cursor_flags & CF_TELETEXT) {
        vdu_ttx_scroll(up);
        return;
    }
    scroll_area(&l, &t, &r, &b);
    scroll_rows(up, l, t, r, b);
}

void vdu_scroll_up(void)
{
    text_scroll(1);
}

void vdu_scroll_down(void)
{
    text_scroll(0);
}

/* ScrollLeft and ScrollRight. They scroll by n bytes, and the column left
 * behind is cleared. */
static void scroll_sideways(int left, int32_t l, int32_t t, int32_t r, int32_t b, uint32_t n)
{
    if (!vdu.screen_ok)
        return;
    if (vdu.cursor_flags & CF_TEUPDATE)
        vdu_set_colours();
    uint32_t w = (uint32_t)(r - l + 1) * vdu.char_width, ll = vdu.mv[MV_LINELENGTH];
    uint8_t *p = cell(l, t);
    if (w > n)
        for (uint32_t y = 0; y < (uint32_t)(b - t + 1) * vdu.row_mult; y++)
            if (left)
                memmove(p + y * ll, p + y * ll + n, w - n);
            else
                memmove(p + y * ll + n, p + y * ll, w - n);
    clear_this_box(left ? cell(r, t) + vdu.char_width - n : p, n, b - t + 1);
}

/* VDU 23,7,m,d,z scrolls the text window (m 0) or the screen (m 1). It
 * scrolls by a character, or across (z 1) by a byte. The direction d is
 * 0-3 for right, left, down or up. The values 4-7 are the same directions
 * as the cursor directions see them. */
void vdu_scroll(uint32_t m, uint32_t d, uint32_t z)
{
    d &= 7;
    if (d >= 4) {
        uint32_t w = d >= 6 ? 0x10103322u : 0x33221010u;
        w >>= (vdu.cursor_flags & 0x0E) << 1;
        d = (w & 15) ^ (d & 1);
    }
    int screen = m >= 1;
    int32_t l = vdu.twl, t = vdu.twt, r = vdu.twr, b = vdu.twb;
    if (VDU_CLIPPING) {        /* ClipScroll */
        if (screen)
            vdu_cbox_full();
        else
            vdu_cbox_text(l, b, r, t);
    }
    if (vdu.cursor_flags & CF_TELETEXT) {       /* by characters. Up and down scroll the
                                                   window (TTXSoftScrollUp/Down) */
        if (d >= 2)
            vdu_ttx_scroll(d == 3);
        else
            vdu_ttx_scroll_side(d == 1, screen);
        return;
    }
    if (screen)
        l = t = 0, r = (int32_t)vdu.mv[MV_SCRRCOL], b = (int32_t)vdu.mv[MV_SCRBROW];
    if (d >= 2) {
        if (!screen)                    /* ScrollUp uses the window, or the screen if none */
            scroll_area(&l, &t, &r, &b);
        scroll_rows(d == 3, l, t, r, b);
        return;
    }
    uint32_t sh = vdu.mv[MV_LOG2BPC];
    if (z)
        sh -= vdu.mv[MV_LOG2BPP] - ((vdu.mv[MV_FLAGS] & MF_BBCGAP) ? 1 : 0);
    scroll_sideways(d == 1, l, t, r, b, 1u << sh);
}

/* OS_Byte 135 reads the character in the cell at the cursor, or 0 if none.
 * As ReadCharacter does, each pixel is compared with its lane of
 * TextBgColour. The kernel exclusive-ORs whole words of the screen with
 * it. The exception is at 1 bpc, where it packs four rows' bytes into a
 * word, the first on top, so a row's lane is 3 - (row & 3).
 * Double-vertical rows must match in pairs. */
static uint32_t read_char_now(void)
{
    if (vdu.cursor_flags & CF_TELETEXT)
        return vdu_ttx_read_char();
    if (!vdu.screen_ok)
        return 0;
    if (vdu.cursor_flags & CF_TEUPDATE)
        vdu_set_colours();
    uint8_t rows[8];
    int split = (vdu.cursor_flags & CF_SPLIT) != 0;     /* ReadCharacter: the
                                                           input cursor's cell */
    const uint8_t *p = cell(split ? vdu.icx : vdu.cx, split ? vdu.icy : vdu.cy);
    uint32_t ll = vdu.mv[MV_LINELENGTH], bpc = vdu.bpc, n = bpc / 8;
    uint32_t pm = bpc >= 32 ? ~0u : (1u << bpc) - 1;
    int dbl = (vdu.mv[MV_FLAGS] & MF_DOUBLEVERTICAL) != 0;
    for (uint32_t r = 0; r < 8; r++) {
        const uint8_t *q = p + (dbl ? 2 * r : r) * ll;
        if (dbl && memcmp(q, q + ll, vdu.char_width))
            return 0;                   /* RdCh1BitDouble: "bad character" */
        uint32_t bits = 0;
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t at = x * bpc / 8, v = 0, lane;
            if (bpc == 1)
                lane = 3 - (r & 3);
            else
                lane = (uint32_t)((uintptr_t)(q + at) & 3);
            uint32_t bg = vdu.text_bg >> (8 * lane);
            if (bpc >= 8)
                memcpy(&v, q + at, n);
            else
                v = q[at] >> (x * bpc % 8), bg >>= x * bpc % 8;
            if ((v & pm) != (bg & pm))
                bits |= 0x80u >> x;
        }
        rows[r] = (uint8_t)bits;
    }
    for (uint32_t c = 32; c < 256; c++)
        if (c != 127 && !memcmp(rows, vdu.font[c - 32], 8))
            return c;
    return 0;
}

/* ReadCharacter takes the cursors off the screen first, and it must. They
 * are drawn by exclusive-OR, so a cursor standing on the cell inverts the
 * very pixels that are about to be compared with the font, and the
 * character reads as none. While editing, that happens on every other
 * flash, so half of COPY's presses answer with a beep. */
uint32_t vdu_read_char(void)
{
    vdu_pre_wrch();
    uint32_t c = read_char_now();
    vdu_post_wrch();
    return c;
}

/* ---- the cursor ---------------------------------------------------------------- */

/* EORCursor. This exclusive-ORs rows [start, end) of one cell with
 * CursorFill. */
static void eor_cell(int32_t col, int32_t row, uint32_t start, uint32_t end)
{
    if (!vdu.screen_ok || start >= end)
        return;
    uint8_t *p = cell(col, row);
    uint32_t ll = vdu.mv[MV_LINELENGTH];
    int banks = vdu.cursor_flags & CF_TELETEXT ? 2 : 1;     /* CursorTeletext draws in both */
    for (int k = 0; k < banks; k++, p += vdu.mv[MV_SCREENSIZE] / 2)
        for (uint32_t y = start; y < end; y++)
            for (uint32_t i = 0; i < vdu.char_width; i++)
                p[y * ll + i] ^= (uint8_t)vdu.cursor_fill;
    if (vdu.vd) {                       /* a virtual display: whoever shows it must see the cell */
        int32_t l = col * (int32_t)vdu.tchar_x;
        int32_t t = (int32_t)vdu.mv[MV_YWIND] - row * (int32_t)vdu.row_mult;
        if (banks == 2)
            vdisplay_dirty(0, 0, (int32_t)vdu.mv[MV_XWIND], (int32_t)vdu.mv[MV_YWIND]);
        else
            vdisplay_dirty(l, t - (int32_t)vdu.row_mult + 1, l + (int32_t)vdu.tchar_x - 1, t);
    }
}

/* The flashing cursor. While the cursors are split, the input one flashes
 * ("flashing cursor is at input", in EORFlashCursor's caller). The place
 * where typing goes is shown by the steady block below. */
void vdu_cursor_eor(void)
{
    int split = (vdu.cursor_flags & CF_SPLIT) != 0;
    eor_cell(split ? vdu.icx : vdu.cx, split ? vdu.icy : vdu.cy,
             vdu.cur_start, vdu.cur_end);
}

/* DoOutputCursor. This shows where the typing goes, while the cursors are
 * split. It is a whole cell, not the cursor's own rows, and it does not
 * flash. */
static void eor_output_block(void)
{
    eor_cell(vdu.cx, vdu.cy, 0, vdu.row_mult);
}

/* ProgReg10. This programs the 6845's register 10, which sets the start
 * row and whether the cursor is steady, off or flashing. */
void vdu_prog_reg10(uint32_t v, int copy)
{
    if (copy)
        vdu.reg10copy = v;
    uint32_t m = v & 0x60;
    if (m < 0x40) {
        vdu.cur_counter = 0;                    /* frozen */
        vdu.cur_desired = m & 0x20 ? 0 : CF_ACTUAL;
    } else {
        vdu.cur_speed = m & 0x20 ? 16 : 8;
        if (!vdu.cur_counter)
            vdu.cur_counter = 1;
    }
    uint32_t start = v & 0x1F;
    if (vdu.cursor_flags & CF_TELETEXT)
        start >>= 1;
    if (vdu.mv[MV_FLAGS] & MF_DOUBLEVERTICAL)
        start <<= 1;
    vdu.cur_start = start > vdu.row_mult ? vdu.row_mult : start;
}

/* VDU 23,0,11 sets the end row. */
void vdu_cursor_end(uint32_t v)
{
    if (vdu.cursor_flags & CF_TELETEXT)
        v >>= 1;
    v++;
    if (vdu.mv[MV_FLAGS] & MF_DOUBLEVERTICAL)
        v <<= 1;
    vdu.cur_end = v > vdu.row_mult ? vdu.row_mult : v;
}

void vdu_cursor_init(void)
{
    vdu_prog_reg10(vdu.cursor_flags & CF_TELETEXT ? 0x72 : 0x67, 1);
    vdu.cur_counter = 1;
    if (vdu.dest_sprite)                /* with output to a sprite, the cursor is off */
        vdu_prog_reg10(0x20, 1);
    vdu.cur_end = vdu.row_mult;
}

/* ---- cursor editing (DoCursorEdit, s/vdu/vducursoft) ----------------------------
 *
 * This is the screen editor that RISC OS inherits from the BBC Micro. A
 * cursor key splits the cursor in two. The place where typing goes stays
 * where it is, shown as a steady block, and a second flashing cursor
 * moves about the screen. COPY reads the character under that second
 * cursor and hands it to whoever is reading a line, exactly as if it had
 * been typed. It then moves on one place. So a line that is already on
 * the screen is copied back a character at a time, or picked over and
 * retyped in part. Editing ends when a carriage return is written
 * (vdu_cursor_unsplit, below).
 *
 * None of it is BASIC's. It is in the OS, under OS_ReadC, so every line
 * read in the box gets it. That includes the * prompt, BASIC's > prompt,
 * INPUT and *Build.
 */

/* VDUBE. This tells whether a cursor move is allowed just now. It is not
 * allowed in the middle of a VDU sequence, or in VDU 5 mode, where there is
 * no text cursor to move. */
static int may_move(void)
{
    return vdu.qwant == 0 && !(vdu.cursor_flags & CF_VDU5);
}

/* The input cursor moves one place on, wrapping inside the text window.
 * Off the left edge is the right of the line above, and off the right is
 * the left of the line below. The top and bottom wrap into each other. The
 * screen never scrolls for it. */
static void input_up(void)
{
    if (--vdu.icy < vdu.twt)
        vdu.icy = vdu.twb;
}

static void input_down(void)
{
    if (++vdu.icy > vdu.twb)
        vdu.icy = vdu.twt;
}

static void input_left(void)
{
    if (--vdu.icx < vdu.twl) {
        vdu.icx = vdu.twr;
        input_up();
    }
}

/* InputCursorHT. COPY moves the cursor on by this too. */
static void input_right(void)
{
    if (++vdu.icx > vdu.twr) {
        vdu.icx = vdu.twl;
        input_down();
    }
}

void vdu_cursor_unsplit(void)
{
    if (!(vdu.cursor_flags & CF_SPLIT))
        return;
    vdu_pre_wrch();                     /* both cursors off while it changes */
    vdu.cursor_flags &= ~CF_SPLIT;
    vdu_prog_reg10(vdu.reg10copy, 0);   /* the flash back to its own rate */
    vdu_post_wrch();
}

int ros_vdu_cursor_edit(uint32_t code, uint8_t *out)
{
    if (code == 0x87) {                 /* COPY */
        uint32_t ch = 0;
        if ((vdu.cursor_flags & (CF_SPLIT | CF_VDU5)) == CF_SPLIT)
            ch = vdu_read_char();       /* under the input cursor */
        if (ch == 0) {
            int plain;                  /* BadCopyExit: a beep, and no character */
            ros_vdu_write(7, &plain);
            return 0;
        }
        if (may_move()) {
            vdu_pre_wrch();
            input_right();
            vdu_post_wrch();
        }
        *out = (uint8_t)ch;
        return 1;
    }
    if (!may_move())
        return 0;
    vdu_pre_wrch();
    if (!(vdu.cursor_flags & CF_SPLIT)) {
        vdu.icx = vdu.cx, vdu.icy = vdu.cy;
        vdu.cursor_flags |= CF_SPLIT;
        /* ICM10. The flash rate is doubled while editing. The rate it was
         * is kept in reg10copy for vdu_cursor_unsplit to put back. */
        vdu_prog_reg10(vdu.reg10copy & ~0x20u, 0);
    }
    switch (code) {
    case 0x88: input_left(); break;
    case 0x89: input_right(); break;
    case 0x8A: input_down(); break;
    default:   input_up(); break;       /* &8B */
    }
    vdu_post_wrch();
    return 0;                           /* the key is used up */
}

/* PreWrchCursor. This takes the cursor off the screen and keeps the VSync
 * off it. When the cursors are split, the steady block comes off too, on
 * every entry. It is always there, where the flashing one is there only
 * half the time. */
void vdu_pre_wrch(void)
{
    uint32_t was = vdu.cursor_flags & CF_INWRCH ? 1u : 0u;
    vdu.cursor_flags |= CF_INWRCH;
    vdu.cur_stack = vdu.cur_stack >> 1 | was << 31;
    if (was)
        return;
    if (vdu.cursor_flags & CF_SPLIT)
        eor_output_block();
    if (!(vdu.cursor_flags & CF_ACTUAL))
        return;
    vdu.cursor_flags &= ~CF_ACTUAL;
    vdu_cursor_eor();
}

/* PostWrchCursor. This puts the cursor back as the flashing wants it. */
void vdu_post_wrch(void)
{
    uint32_t still = vdu.cur_stack >> 31;
    vdu.cur_stack <<= 1;
    if (still)
        return;
    if ((vdu.cur_desired ^ vdu.cursor_flags) & CF_ACTUAL) {
        vdu.cursor_flags ^= CF_ACTUAL;
        vdu_cursor_eor();
    }
    if (vdu.cursor_flags & CF_SPLIT)
        eor_output_block();
    vdu.cursor_flags &= ~CF_INWRCH;
}

/* The VSync. This handles the pointer, then the flashing of the real
 * display and of each virtual one (vdisplay.c). */
void ros_vdu_vsync(void)
{
    vdu_pointer_vsync();
    vdu_flash_vsync();
    vdisplay_vsync();
}

void vdu_flash_vsync(void)
{
    if (vdu.cursor_flags & CF_TELETEXT)
        vdu_ttx_vsync();                /* the flash banks */
    if (vdu.cur_counter && --vdu.cur_counter == 0) {
        vdu.cur_counter = vdu.cur_speed;
        vdu.cur_desired ^= CF_ACTUAL;
    }
    vdu_palette_flash();
    if (vdu.cursor_flags & CF_INWRCH)
        return;
    if ((vdu.cur_desired ^ vdu.cursor_flags) & CF_ACTUAL) {
        vdu.cursor_flags ^= CF_ACTUAL;
        vdu_cursor_eor();
    }
}
