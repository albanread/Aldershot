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
 * (Sources/Kernel: s.vdu.vdudriver, s.vdu.vduwrch, s.vdu.vdu23,
 * s.PMF.osbyte, s.vdu.vducursoft).
 */
/* vdu.c -- the VDU stream: the queue, the control codes, VDU 23, and the
 * VDU's OS_Bytes and OS_Words.
 *
 * This follows the kernel's Kernel/s/vdu/vdudriver (Vdu, VduJTb and
 * VduQTb), vduwrch, vdu23 and PMF/osbyte.
 *
 *   - A control code collects its parameters first and then runs. The
 *     number of parameters is 1 for code 1, 17 and 22, 2 for 18 and 31, 5
 *     for 19 and 25, 9 for 23, 8 for 24, and 4 for 28 and 29. OS_Byte 218
 *     reads and writes how many are still wanted, as minus the count.
 *   - Every sequence and every character runs with the cursor taken off
 *     the screen (PreWrchCursor and PostWrchCursor).
 *   - VDU 21 stops everything but VDU 6, though sequences are still
 *     collected. VDU 2 and 3 only mark the printer on or off, because
 *     there is no printer.
 *   - A character is painted at the cursor, which then moves right. Past
 *     the window's right edge the cursor wraps, and past its bottom the
 *     window scrolls. The scroll happens at once, unless VDU 23,16 sets
 *     bit 0. Then the newline waits for the next character (C81Bit).
 *   - VDU 8, 9, 10, 11, 13, 30, 31 and 127 move the cursor as the kernel's
 *     do for the usual directions, which are left to right and top to
 *     bottom.
 *
 * PLOT is in plot.c and shapes.c. VDU 5's characters are in vdu5.c.
 *
 * VDU 23,7 scrolls (text.c) and VDU 23,8 clears blocks as the kernel's do,
 * cursor directions and all. In teletext (ttx.c) characters go into the
 * page's map. COLOUR, VDU 19 and VDU 20 do nothing there. VDU 23,18 sets
 * teletext's transparency, suspends drawing, reveals and enables black.
 * Not done yet: the cursor's own movement in directions other than the
 * usual (VDU 23,16 bits 1-4), page mode's waiting for Shift, and cursor
 * editing.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/international.h"
#include "rosgd/keyboard.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "vduws.h"

#define UKVDU23V 0x17u

/* The parameters that each control code collects (VduQTb). */
static const uint8_t queue_count[32] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 5, 0, 0, 1, 9, 8, 5, 0, 0, 4, 4, 0, 2,
};

static uint8_t q(int i)
{
    return (uint8_t)ros_ld8(vdu.qq + (uint32_t)i);
}

static int32_t q16(int i)
{
    return (int16_t)(q(i) | q(i + 1) << 8);
}

/* ---- the cursor's movements ------------------------------------------------ */

/* PageTest. This counts the lines. Waiting for Shift in page mode is not
 * done yet. */
static void page_test(void)
{
    vdu.page_lines++;
}

static void page_back(void)
{
    if (vdu.page_lines)
        vdu.page_lines--;
}

static void cr(void)
{
    vdu.cursor_flags &= ~CF_C81;
    vdu.cx = vdu.twl;
}

static void lf(void)
{
    page_test();
    if (vdu.cy + 1 <= vdu.twb)
        vdu.cy++;
    else
        vdu_scroll_up();
}

static void vt(void)
{
    page_back();
    if (vdu.cy - 1 >= vdu.twt)
        vdu.cy--;
    else
        vdu_scroll_down();
}

static void bs(void)
{
    if (vdu.cursor_flags & CF_C81) {    /* the pending newline is undone */
        vdu.cursor_flags &= ~CF_C81;
        return;
    }
    if (vdu.cx - 1 >= vdu.twl) {
        vdu.cx--;
        return;
    }
    page_back();
    vdu.cx = vdu.twr;
    if (vdu.cy - 1 >= vdu.twt)
        vdu.cy--;
    else
        vdu_scroll_down();
}

static void ht(void)
{
    if (vdu.cursor_flags & CF_C81) {    /* the pending newline comes first */
        cr();
        lf();
    }
    if (vdu.cx + 1 <= vdu.twr) {
        vdu.cx++;
        return;
    }
    page_test();
    vdu.cx = vdu.twl;
    if (vdu.cy + 1 <= vdu.twb)
        vdu.cy++;
    else
        vdu_scroll_up();
}

/* CHT. This moves the cursor after a character. */
static void cht(void)
{
    if (vdu.cx + 1 <= vdu.twr) {
        vdu.cx++;
        return;
    }
    if (vdu.cursor_flags & CF_81COLUMN) {
        vdu.cursor_flags |= CF_C81;
        return;
    }
    page_test();
    vdu.cx = vdu.twl;
    if (vdu.cy + 1 <= vdu.twb)
        vdu.cy++;
    else
        vdu_scroll_up();
}

static void home(void)
{
    vdu.cursor_flags &= ~CF_C81;
    vdu.cx = vdu.twl, vdu.cy = vdu.twt;
}

/* ---- characters ------------------------------------------------------------ */

static void wrch(uint8_t c)
{
    if (vdu.cursor_flags & CF_VDU5) {
        vdu5_char(c);
        return;
    }
    if (vdu.cursor_flags & CF_C81) {
        cr();
        lf();
    }
    if (vdu.cursor_flags & CF_TELETEXT) {       /* TTXWrch */
        if (vdu.cursor_flags & CF_TEUPDATE)
            vdu_set_colours();
        vdu_ttx_wrch(c);
    } else {
        vdu_paint_char(vdu.font[c - 32]);
    }
    if (!(vdu.cursor_flags & CF_NOMOVE))
        cht();
}

static void delete(void)
{
    static const uint8_t blank[8];
    if (vdu.cursor_flags & CF_VDU5) {
        vdu5_delete();
        return;
    }
    if (vdu.cursor_flags & CF_TEUPDATE)
        vdu_set_colours();
    if (!(vdu.cursor_flags & CF_NOMOVE))
        bs();
    if (vdu.cursor_flags & CF_TELETEXT)
        vdu_ttx_wrch(32);               /* it is wiped out with a space */
    else
        vdu_paint_char(blank);
}

/* CursorOnOff. The value 0 is off, 1 is as it was, 2 is steady, and 3 and
 * up are flashing. */
static void cursor_on_off(uint32_t n)
{
    if (n == 0) {
        vdu_prog_reg10(0x20, 0);
        return;
    }
    uint32_t v = vdu.reg10copy;
    if (n == 1) {
        vdu_prog_reg10(v, 0);
        return;
    }
    v = n == 2 ? v & ~0x60u : v | 0x60u;
    vdu_prog_reg10(v, 1);
}

/* ---- the single-byte codes ----------------------------------------------------- */

/* VDU 7, the bell (Kernel s/vdu/vduwrch's BEL). It calls OS_Word 7, SOUND,
 * with the channel, information, pitch and duration of OS_Byte 211-214.
 * In the information byte, bits 0-1 are the S bits and bit 2 is the H bit
 * (bit 4 of the block's flags). Bits 3-7 are the amplitude less one, which
 * is signed. The default &90 is -15+2 = -13, and &D0, a quiet bell, is -5
 * (#88). */
static void bel(void)
{
    static uint32_t block;
    if (!block)
        block = ros_addr(ros_rma_alloc(8));
    uint8_t *b = ros_ptr(block);
    uint32_t info = ros_byte_var(0xD4);
    int32_t amp = ((int32_t)(int8_t)(uint8_t)info >> 3) + 1;
    b[0] = ros_byte_var(0xD3);
    b[1] = (uint8_t)((info & 3) | (info & 4 ? 0x10 : 0));
    b[2] = (uint8_t)amp, b[3] = (uint8_t)((uint32_t)amp >> 8);
    b[4] = ros_byte_var(0xD5), b[5] = 0;
    b[6] = ros_byte_var(0xD6), b[7] = 0;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 7, s.r[1] = block;
    ros_swi(&s, XOS_Word);
}

static void control(uint8_t c)
{
    uint32_t flags = vdu.mv[MV_FLAGS];
    if ((vdu.cursor_flags & CF_VDU5) && ((c >= 8 && c <= 13) || c == 30)) {
        vdu5_control(c);
        return;
    }
    switch (c) {
    case 2: vdu.status |= VS_VDU2; break;
    case 3: vdu.status &= ~VS_VDU2; break;
    case 4:
        if (flags & MF_NONGRAPHIC)
            break;
        vdu.cursor_flags &= ~CF_VDU5;
        cursor_on_off(1);
        break;
    case 5:
        if (flags & MF_NONGRAPHIC)
            break;
        vdu.cursor_flags |= CF_VDU5;
        vdu_prog_reg10(0x20, 0);
        break;
    case 7: bel(); break;
    case 8: bs(); break;
    case 9: ht(); break;
    case 10: lf(); break;
    case 11: vt(); break;
    case 12: vdu_cls(); break;
    case 13:                            /* Vdu15: a carriage return ends editing */
        vdu_cursor_unsplit();
        cr();
        break;
    case 14:
        vdu.cursor_flags |= CF_PAGEMODE;
        vdu.page_lines = 0;
        break;
    case 15: vdu.cursor_flags &= ~CF_PAGEMODE; break;
    case 16: vdu_clg(); break;
    case 20:                            /* neither in teletext */
        if (!(vdu.dmv[MV_FLAGS] & MF_TELETEXT))
            vdu_palette_default();
        if (!(vdu.cursor_flags & CF_TELETEXT))
            vdu_default_colours();
        break;
    case 21: vdu.cursor_flags |= CF_DISABLED; break;
    case 26: vdu_default_windows(); break;
    case 30: home(); break;
    case 127: delete(); break;
    default: break;                     /* 0, 6, 7 (no sound) and 27 */
    }
}

/* ---- VDU 23 ---------------------------------------------------------------------- */

/* UnknownVdu23. An error from the vector is the VDU's (VduBadExit). */
static os_error *ukvdu23(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = q(0), s.r[1] = vdu.qq;
    ros_vector_call(UKVDU23V, &s);
    return s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
}

static os_error *vdu23_17(void)
{
    uint32_t t = q(2);
    switch (q(1)) {
    case 0:
    case 1: {                           /* The new tint is kept. The colour is compiled
                                           again only when its top two bits change,
                                           at 8 bpp and up (Vdu23_17). */
        uint32_t *tint = q(1) == 0 ? &vdu.tftint : &vdu.tbtint, old = *tint;
        *tint = t;
        if (!((old ^ t) & 0xC0) || vdu.bpp < 8)
            break;
        if (q(1) == 0)
            vdu_compile_fg();
        else
            vdu_compile_bg();
        vdu.cursor_flags |= CF_TEUPDATE;
        break;
    }
    case 2: vdu.gftint = t, vdu_set_colour(); break;
    case 3: vdu.gbtint = t, vdu_set_colour(); break;
    case 5: {                           /* swap the text colours */
        uint32_t x = vdu.tfore;
        vdu.tfore = vdu.tback, vdu.tback = x;
        x = vdu.text_fg, vdu.text_fg = vdu.text_bg, vdu.text_bg = x;
        if (vdu.mv[MV_NCOLOUR] >= 63)
            x = vdu.tftint, vdu.tftint = vdu.tbtint, vdu.tbtint = x;
        vdu.cursor_flags |= CF_TEUPDATE;
        break;
    }
    case 7:                             /* VDU 5's sizes. Bit 0, VDU 4's, is ignored. */
        if (q(2) & 2)
            vdu.gchar_sx = (uint32_t)q16(3), vdu.gchar_sy = (uint32_t)q16(5);
        if (q(2) & 4)
            vdu.gchar_spx = (uint32_t)q16(3), vdu.gchar_spy = (uint32_t)q16(5);
        break;
    case 4:                             /* BBC (0) or native ECF bytes */
        vdu.bbc_ecfs = t;
        break;
    case 6:                             /* the ECF origin */
        vdu_ecf_origin(q16(2), q16(4));
        break;
    default:
        return ukvdu23();
    }
    return NULL;
}

/* ---- VDU 23,8: clear a block of text ---------------------------------------------- */

/* CP80. This gives the cursor from the window's "top left", as the cursor
 * directions (CursorFlags bits 1-3) see it. WBotRig is the window's
 * "bottom right". */
static void user_cursor(int32_t *x, int32_t *y)
{
    uint32_t f = vdu.cursor_flags;
    int32_t a = f & 2 ? vdu.twr - vdu.cx : vdu.cx - vdu.twl;
    int32_t b = f & 4 ? vdu.twb - vdu.cy : vdu.cy - vdu.twt;
    if (f & 8)
        *x = b, *y = a;
    else
        *x = a, *y = b;
}

/* RowClear. This clears columns x0 <= X < x1 of row y, in the user's
 * terms. */
static void row_clear(int32_t y, int32_t x0, int32_t x1)
{
    uint32_t f = vdu.cursor_flags;
    x1 -= 1;
    if ((uint32_t)x0 > (uint32_t)x1)
        return;
    int32_t l = x0, b = y, r = x1, t = y;
    if (f & 8)
        l = y, b = x1, r = y, t = x0;
    int32_t cl = f & 2 ? vdu.twr - r : vdu.twl + l, cr = f & 2 ? vdu.twr - l : vdu.twl + r;
    int32_t cb = f & 4 ? vdu.twb - t : vdu.twt + b, ct = f & 4 ? vdu.twb - b : vdu.twt + t;
    vdu_clear_box(cl, ct, cr, cb);
}

/* VDU 23,8,t1,t2,x1,y1,x2,y2 clears from t1's point offset by x1,y1 up to
 * t2's point offset by x2,y2. Bits 0-1 of t choose the window's left, the
 * cursor's column or off the right. Bits 2-3 choose the window's top, the
 * cursor's row or its bottom. Each point is clamped to the window, and the
 * end must come after the start. */
static void clear_block(void)
{
    static uint8_t cbws[12];            /* CBWS, CBStart, CBEnd */
    int32_t x, y;
    user_cursor(&x, &y);
    if (vdu.cursor_flags & CF_C81)
        x++;
    cbws[0] = cbws[1] = 0;
    cbws[2] = (uint8_t)x, cbws[3] = (uint8_t)y;
    uint32_t f = vdu.cursor_flags;
    int32_t w = vdu.twr - vdu.twl, h = vdu.twb - vdu.twt;
    cbws[4] = (uint8_t)((f & 8 ? h : w) + 1), cbws[5] = (uint8_t)(f & 8 ? w : h);
    for (int k = 0; k < 4; k++) {
        uint32_t t = q(1 + k / 2), i = k & 1 ? (t >> 1) | 1 : (t & 3) << 1;
        int32_t v = (i < sizeof cbws ? cbws[i] : 0) + (int8_t)q(3 + k);
        uint8_t max = cbws[4 + (k & 1)];
        if (v < 0)
            v = 0;
        if ((uint32_t)v > max)
            v = max;
        cbws[8 + k] = (uint8_t)v;
    }
    uint32_t sx = cbws[8], sy = cbws[9], ex = cbws[10], ey = cbws[11];
    if (ey < sy || (ey == sy && ex <= sx))
        return;
    int32_t from = (int32_t)sx;
    for (uint32_t row = sy; row != ey; row++, from = 0)
        row_clear((int32_t)row, from, cbws[4]);
    row_clear((int32_t)ey, from, (int32_t)ex);
}

static os_error *vdu23(void)
{
    uint32_t c = q(0);
    if (c >= 32) {
        for (int i = 0; i < 8; i++)
            vdu.font[c - 32][i] = q(1 + i);
        return NULL;
    }
    switch (c) {
    case 0: {
        uint32_t reg = q(1) & 31, v = q(2);
        if (reg == 9 || reg == 11)
            vdu_cursor_end(v);
        else if (reg == 10)
            vdu_prog_reg10(v, 1);
        else if (reg >= 12)
            return ukvdu23();
        break;                          /* 0-7 ignored; 8 interlace, none */
    }
    case 1:
        if (!(vdu.cursor_flags & CF_VDU5))
            cursor_on_off(q(1));
        break;
    case 9:
    case 10: {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = q(1);
        vdu_palette_period(c == 10, &s);
        break;
    }
    case 16: {
        uint32_t low = ((vdu.cursor_flags & 0xFF) & q(2)) ^ q(1);
        vdu.cursor_flags = (vdu.cursor_flags & ~0xFFu) | low;
        if (!(low & CF_81COLUMN) && (vdu.cursor_flags & CF_C81)) {
            cr();                       /* the pending newline, released */
            lf();
        }
        break;
    }
    case 17: return vdu23_17();
    case 2: case 3: case 4: case 5:
    case 6:
    case 11:
    case 12: case 13: case 14: case 15: {
        uint8_t b[8];
        for (int i = 0; i < 8; i++)
            b[i] = q(1 + i);
        if (c == 6)
            vdu_line_style(b);
        else if (c == 11)
            vdu_ecf_default();
        else if (c <= 5)
            vdu_ecf_complex(c - 2, b);
        else
            vdu_ecf_simple(c - 12, b);
        break;
    }
    case 27:                            /* 0 selects and 1 gets sprite "n" */
        vdu_vdu23_27(q(1), q(2));
        break;
    case 7:
        vdu_scroll(q(1), q(2), q(3));
        break;
    case 8:
        clear_block();
        break;
    case 18:                            /* teletext's codes 0-3 */
        if (!vdu_ttx_vdu23_18(q(1), q(2)))
            return ukvdu23();
        break;
    default:
        return ukvdu23();
    }
    return NULL;
}

/* ---- the codes with parameters ---------------------------------------------------- */

static os_error *queued(void)
{
    uint32_t nc = vdu.mv[MV_NCOLOUR];
    switch (vdu.qcode) {
    case 17: {                          /* COLOUR */
        if (vdu.cursor_flags & CF_TELETEXT)
            break;
        uint32_t n = q(0), c = n & nc & 63;
        if (n < 128) {
            if (c != vdu.tfore) {
                vdu.tfore = c;
                vdu_compile_fg();
                vdu.cursor_flags |= CF_TEUPDATE;
            }
        } else if (c != vdu.tback) {
            vdu.tback = c;
            vdu_compile_bg();
            vdu.cursor_flags |= CF_TEUPDATE;
        }
        break;
    }
    case 18: {                          /* GCOL */
        uint32_t a = q(0), c = q(1);
        if (c & 0x80)
            vdu.gplbmd = a, vdu.gbcol = c & nc;
        else
            vdu.gplfmd = a, vdu.gfcol = c & nc;
        vdu_set_colour();
        break;
    }
    case 19: {
        uint8_t p[5];
        for (int i = 0; i < 5; i++)
            p[i] = q(i);
        vdu_palette_vdu19(p);
        break;
    }
    case 22:
        return vdu_set_mode(q(0));
    case 23:
        return vdu23();
    case 24: {                          /* the graphics window */
        int32_t xe = (int32_t)vdu.mv[MV_XEIG], ye = (int32_t)vdu.mv[MV_YEIG];
        int32_t l = (q16(0) + vdu.orgx) >> xe, b = (q16(2) + vdu.orgy) >> ye;
        int32_t r = (q16(4) + vdu.orgx) >> xe, t = (q16(6) + vdu.orgy) >> ye;
        if (r >= l && t >= b && l >= 0 && b >= 0 && t <= (int32_t)vdu.mv[MV_YWIND] &&
            r <= (int32_t)vdu.mv[MV_XWIND])
            vdu.gwl = l, vdu.gwb = b, vdu.gwr = r, vdu.gwt = t;
        break;
    }
    case 25:
        return vdu_plot(q(0), q16(1), q16(3));
    case 28: {                          /* the text window */
        int32_t l = q(0), b = q(1), r = q(2), t = q(3);
        vdu.status |= VS_WINDOWING;
        if (l > r || r > (int32_t)vdu.mv[MV_SCRRCOL] || t > b || b > (int32_t)vdu.mv[MV_SCRBROW])
            break;
        vdu.twl = l, vdu.twb = b, vdu.twr = r, vdu.twt = t;
        if (vdu.cx < l || vdu.cx > r || vdu.cy < t || vdu.cy > b)
            home();
        break;
    }
    case 29:                            /* the graphics origin */
        vdu.orgx = q16(0), vdu.orgy = q16(2);
        vdu_ieg();
        break;
    case 31: {                          /* TAB */
        if (vdu.cursor_flags & CF_VDU5) {
            vdu5_tab(q(0), q(1));
            break;
        }
        int32_t x = vdu.twl + q(0), y = vdu.twt + q(1);
        if (y > vdu.twb)
            break;
        if (x > vdu.twr) {
            if ((vdu.cursor_flags & CF_81COLUMN) && x == vdu.twr + 1) {
                vdu.cx = vdu.twr, vdu.cy = y;
                vdu.cursor_flags |= CF_C81;
            }
            break;
        }
        vdu.cx = x, vdu.cy = y;
        vdu.cursor_flags &= ~CF_C81;
        break;
    }
    default:
        break;                          /* 1: the printer's, and there is none */
    }
    return NULL;
}

/* ---- the stream ------------------------------------------------------------------- */

os_error *ros_vdu_write(uint8_t c, int *plain)
{
    os_error *e = NULL;
    *plain = 0;
    int disabled = (vdu.cursor_flags & CF_DISABLED) != 0;
    if (vdu.qwant) {
        /* OS_Byte 218 or a forged save area can leave qlen anything. The
         * queue is a 16-byte RMA block. */
        if ((uint32_t)vdu.qlen < 16)
            ros_st8(vdu.qq + (uint32_t)vdu.qlen, c);
        vdu.qlen++;
        if (--vdu.qwant || disabled)
            return NULL;
        vdu_pre_wrch();
        e = queued();
        vdu_post_wrch();
        return e;
    }
    if (c >= 32 && c != 127) {
        if (disabled)
            return NULL;
        *plain = 1;
        vdu_pre_wrch();
        wrch(c);
        vdu_post_wrch();
        return NULL;
    }
    if (c < 32 && queue_count[c]) {
        vdu.qcode = c;
        vdu.qlen = 0;
        vdu.qwant = queue_count[c];
        return NULL;
    }
    if (disabled) {
        if (c == 6)
            vdu.cursor_flags &= ~CF_DISABLED;
        return NULL;
    }
    *plain = c == 7 || c == 8 || c == 10 || c == 13 || c == 127;
    vdu_pre_wrch();
    control(c);
    vdu_post_wrch();
    return NULL;
}

/* ---- OS_Byte and OS_Word -------------------------------------------------------------- */

static void reset_font(uint32_t first, uint32_t last)
{
    for (uint32_t c = first; c <= last; c++)
        memcpy(vdu.font[c - 32], ros_vdu_hard_font[c - 32], 8);
}

/* ResetPartFont resets pages (32 characters) first to first + count - 1 of
 * the soft font. The request is offered to the International module first
 * as the current alphabet's (Service_International 5). The hard font is
 * used only if no one claims it. R1 and R2 come back as the kernel leaves
 * them. If the request was claimed they are the page and the count. If
 * not, they are DoResetFont's. R2 is the last word it copied, which is the
 * last character's bottom four rows. R1 is the address past the hard
 * font's bytes that it copied. That address is in the kernel's ROM, which
 * ROSGD's font is not in, so here it is all ones. */
static void reset_part_font(struct ros_cpu *s, uint32_t first, uint32_t count)
{
    uint32_t last = (first << 5) + (count << 5) - 1;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = SERVICE_INTERNATIONAL, c.r[2] = INTER_DEFINE, c.r[3] = ros_keyboard_alphabet();
    c.r[4] = first << 5, c.r[5] = last;
    ros_service_call(&c);
    if (c.r[1] == 0) {
        s->r[1] = first, s->r[2] = count;
        return;
    }
    reset_font(first << 5, last);
    const uint8_t *w = ros_vdu_hard_font[last - 32] + 4;
    s->r[1] = 0xFFFFFFFFu;
    s->r[2] = (uint32_t)w[0] | (uint32_t)w[1] << 8 | (uint32_t)w[2] << 16 | (uint32_t)w[3] << 24;
}

/* POS and VPOS. This gives the output cursor within the window. */
static void pos(struct ros_cpu *s)
{
    s->r[1] = (uint32_t)(vdu.cx - vdu.twl) + (vdu.cursor_flags & CF_C81 ? 1 : 0);
    s->r[2] = (uint32_t)(vdu.cy - vdu.twt);
}

/* OS_Byte 112 and 113 on the real screen. They set the screen bank that
 * the VDU draws in and the one that is shown (the kernel's DoSetDriverBank
 * and DoSetDisplayBank, s/vdu/vdu23). Bank 0 is bank 1. A bank that does
 * not fit in the screen's memory is ignored. R1 is the old value either
 * way. OS_Byte 250 and 251 read it too (MemDriver and MemDisplay, which are
 * 0 for the default since the mode changed). Bank n starts (n - 1) screens
 * past the framestore's start. DRMVideo makes room for three. ROSGD never
 * scrolls the display in hardware, so a bank's start is its current start.
 * The VDU draws in the new bank from here, moving ScreenStart only when
 * output is to the screen (NewScreenStart). The display is moved through
 * GraphicsV SetDMAAddress, as the kernel's SetVinit moves it, and DRMVideo
 * shows it at the next frame. */
#define GV_SET_DMA_ADDRESS 6u

static int screen_bank(struct ros_cpu *s)
{
    uint32_t var = s->r[0] == 112 ? 0xFA : 0xFB;
    uint32_t old = ros_byte_var(var);
    uint32_t bank = s->r[1] & 0xFF, size = vdu.dmv[MV_SCREENSIZE];
    if (bank == 0)
        bank = 1;
    s->r[1] = old;
    if (!vdu.screen_ok || !size || (uint64_t)bank * size > vdu.total_size)
        return 1;                               /* past the end, so it is ignored */
    ros_byte_var_set(var, (uint8_t)bank);
    uint32_t a = vdu.screen_base + (bank - 1) * size;
    if (var == 0xFA) {
        vdu_pre_wrch();
        vdu.dscreen_start = a;
        if (!vdu.dest_sprite) {
            vdu.screen_start = a;
            vdu.screen = ros_ptr(a);
        }
        vdu_post_wrch();
    } else {
        vdu.display_start = a;
        uint32_t r[4] = { 0, a };               /* VInit */
        vdu_graphicsv(GV_SET_DMA_ADDRESS, r);
    }
    return 1;
}

int ros_vdu_byte(struct ros_cpu *s)
{
    if (s->r[0] == 112 || s->r[0] == 113)
        return vdu.vd ? vdisplay_byte(s)        /* a virtual display's two banks */
                      : screen_bank(s);
    switch (s->r[0]) {
    case 9:
    case 10:
        vdu_palette_period(s->r[0] == 10, s);
        return 1;
    case 20:                                    /* characters 32-127 */
        reset_part_font(s, 1, 3);
        return 1;
    case 25:                                    /* 0 all, 1-7 one page */
        if (s->r[1] >= 8)
            s->r[2] = 0;
        else if (s->r[1] == 0)
            reset_part_font(s, 1, 7);
        else
            reset_part_font(s, s->r[1], 1);
        return 1;
    case 106:
        vdu_pointer_select(s);
        return 1;
    case 117: {
        uint32_t f = vdu.cursor_flags, st = 0;
        if (vdu.status & VS_VDU2) st |= 1;
        if (f & CF_PAGEMODE) st |= 4;
        if (vdu.status & VS_WINDOWING) st |= 8;
        if (vdu.status & VS_SHADOW) st |= 16;
        if (f & CF_VDU5) st |= 32;
        if (f & CF_SPLIT) st |= 64;
        if (f & CF_DISABLED) st |= 128;
        s->r[1] = st;
        return 1;
    }
    case 134:
    case 165:
        pos(s);
        return 1;
    case 135:
        s->r[1] = vdu_read_char();
        s->r[2] = vdu.mode_no;
        return 1;
    case 160: {
        uint8_t b[17] = { 0 };
        int32_t w[4] = { vdu.gwl, vdu.gwb, vdu.gwr, vdu.gwt };
        for (int i = 0; i < 4; i++)
            b[2 * i] = (uint8_t)w[i], b[2 * i + 1] = (uint8_t)(w[i] >> 8);
        b[8] = (uint8_t)vdu.twl, b[9] = (uint8_t)vdu.twb;
        b[10] = (uint8_t)vdu.twr, b[11] = (uint8_t)vdu.twt;
        b[12] = (uint8_t)vdu.orgx, b[13] = (uint8_t)(vdu.orgx >> 8);
        b[14] = (uint8_t)vdu.orgy, b[15] = (uint8_t)(vdu.orgy >> 8);
        if (s->r[1] < 16)
            s->r[2] = b[s->r[1] + 1], s->r[1] = b[s->r[1]];
        return 1;
    }
    case 163:                           /* with R1 242, the dot-dash pattern's length */
        if (s->r[1] != 242)
            return 0;
        vdu_dot_length(s);
        return 1;
    case 218: {                         /* the queue, as minus the bytes still wanted */
        uint32_t old = (uint32_t)(-vdu.qwant) & 0xFF;
        uint32_t now = ((old & s->r[2]) ^ s->r[1]) & 0xFF;
        vdu.qwant = (int)((256 - now) & 0xFF);
        vdu.qlen = vdu.qwant && vdu.qcode < 32 ? queue_count[vdu.qcode] - vdu.qwant : 0;
        if (vdu.qlen < 0)
            vdu.qlen = 0;
        s->r[1] = old;
        s->r[2] = 0;
        return 1;
    }
    default:
        return 0;
    }
}

int ros_vdu_word(struct ros_cpu *s)
{
    uint32_t block = s->r[1];
    switch (s->r[0]) {
    case 9: {                           /* a pixel's colour, or &FF off the window */
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[0] = (uint32_t)(int16_t)(ros_ld8(block) | ros_ld8(block + 1) << 8);
        r.r[1] = (uint32_t)(int16_t)(ros_ld8(block + 2) | ros_ld8(block + 3) << 8);
        ros_thunk_OS_ReadPoint(&r);
        ros_st8(block + 4, r.r[2] & 0xFF);
        return 1;
    }
    case 10: {                          /* a character's definition, or 2-5 the ECFs,
                                           or 6 the dot-dash pattern */
        uint32_t c = ros_ld8(block);
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t b = 0;
            if (c >= 32)
                b = vdu.font[c - 32][i];
            else if (c >= 2 && c <= 5)
                b = vdu.ecf[c - 2][i];
            else if (c == 6)
                b = (uint32_t)(vdu.dot_style >> (8 * i)) & 0xFF;   /* as the kernel keeps it */
            ros_st8(block + 1 + i, b);
        }
        return 1;
    }
    case 11:
        vdu_palette_word11(block);
        return 1;
    case 21:
        vdu_pointer_word(block);
        return 1;
    case 12:
        vdu_palette_word12(block);
        return 1;
    case 13: {                          /* the graphics cursors, previous and current */
        int32_t v[4] = { vdu.oldx, vdu.oldy, vdu.gcsx, vdu.gcsy };
        for (uint32_t i = 0; i < 4; i++) {
            ros_st8(block + 2 * i, (uint32_t)v[i] & 0xFF);
            ros_st8(block + 2 * i + 1, ((uint32_t)v[i] >> 8) & 0xFF);
        }
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- the SWIs --------------------------------------------------------------------- */

void ros_thunk_OS_RemoveCursors(struct ros_cpu *s)
{
    vdu_pre_wrch();
    s->v = 0;
}

void ros_thunk_OS_RestoreCursors(struct ros_cpu *s)
{
    vdu_post_wrch();
    s->v = 0;
}

void ros_thunk_OS_ReadPalette(struct ros_cpu *s)
{
    vdu_palette_read(s);
    s->v = 0;
}
