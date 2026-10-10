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
 * (Sources/Kernel: s.vdu.vdu5).
 */
/* vdu5.c -- VDU 5: characters at the graphics cursor.
 *
 * This follows the kernel's Kernel/s/vdu/vdu5. For sizes other than 8 x 8
 * and 8 x 16 it uses SpriteExtend's scaled character painting
 * (OS_SpriteOp 51, which the kernel hands those sizes to).
 *
 *   - The graphics cursor is the character cell's top-left pixel. Only the
 *     glyph's set bits are plotted, with the foreground colour, GCOL action
 *     and ECF. They are clipped pixel by pixel to the graphics window.
 *   - 8 x 16 doubles each glyph row. Other sizes are SpriteExtend's, as in
 *     the kernel: OS_SpriteOp 51 through SpriteV.
 *   - After a character the cursor moves GCharSpaceX right. Off the
 *     window's right edge it goes to the left edge and down, and off the
 *     bottom it goes to the top. Nothing scrolls. CursorFlags bit 6 stops
 *     the wrapping, and bits 1-3 turn the directions.
 *   - DELETE steps back and clears the cell with the background colour and
 *     the store action.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "vduws.h"
#include "plot.h"

extern const uint8_t ros_vdu_hard_font[224][8];

/* Plots a glyph at the cursor through the table t. */
static void paint(const uint8_t glyph[8], const uint32_t *t, uint32_t glyph_char)
{
    if (!vdu.screen_ok)
        return;
    int32_t sx = (int32_t)vdu.gchar_sx, sy = (int32_t)vdu.gchar_sy;
    int32_t cxl = vdu.gcsix, cyt = vdu.gcsiy;
    const uint32_t *saved = plot_gcol;
    plot_gcol = t;
    if (sx == 8 && (sy == 8 || sy == 16)) {
        int32_t cyb = cyt - sy + 1, cxr = cxl + 7;
        int32_t l = cxl > vdu.gwl ? cxl : vdu.gwl, r = cxr < vdu.gwr ? cxr : vdu.gwr;
        int32_t b = cyb > vdu.gwb ? cyb : vdu.gwb, tp = cyt < vdu.gwt ? cyt : vdu.gwt;
        if (tp - b >= 0 && r - l >= 0 && VDU_CLIPPING)
            vdu_cbox_merge(l, b, r, tp);            /* ClipVdu5 */
        if (tp - b >= 0 && r - l >= 0)
            for (int32_t y = tp; y >= b; y--) {
                int32_t k = cyt - y;
                uint32_t bits = sy == 16 ? glyph[k >> 1] : glyph[k];
                for (int32_t x = l; x <= r; x++)
                    if (bits & (0x80u >> (x - cxl)))
                        plot_write(x, y);
            }
    } else {
        /* SlowVdu5. SpriteExtend paints the glyph (OS_SpriteOp 51),
         * through SpriteV. Errors are ignored, as the kernel ignores them.
         * Its ECF is FgEcfOraEor, so DELETE's background table is copied
         * over that while it runs. */
        uint32_t keep[16];
        if (t != vdu.fg_oe) {
            memcpy(keep, vdu.fg_oe, sizeof keep);
            memcpy(vdu.fg_oe, t, sizeof keep);
        }
        uint32_t *scale = (uint32_t *)ros_ptr(ros_vdu_scratch()) + 56;   /* clear of SpriteExtend's own use */
        scale[0] = (uint32_t)sx, scale[1] = (uint32_t)sy, scale[2] = 8, scale[3] = 8;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 51, c.r[1] = glyph_char, c.r[3] = (uint32_t)vdu.gcsx;
        c.r[4] = (uint32_t)(vdu.gcsy - (int32_t)((uint32_t)(sy - 1) << vdu.mv[MV_YEIG]));
        c.r[6] = ros_addr(scale);
        ros_swi(&c, XOS_SpriteOp);
        if (t != vdu.fg_oe)
            memcpy(vdu.fg_oe, keep, sizeof keep);
    }
    plot_gcol = saved;
}

static int in_window(void)
{
    return plot_in_window(vdu.gcsix, vdu.gcsiy);
}

/* GCursorMove. It returns 1 if the move failed, which means the cursor
 * left the window and may wrap. */
static int cursor_move(uint32_t f)
{
    int was = in_window();
    switch ((f & 0xE) >> 1) {
    case 0: case 2: vdu.gcsix += (int32_t)vdu.gchar_spx; break;
    case 1: case 3: vdu.gcsix -= (int32_t)vdu.gchar_spx; break;
    case 4: case 5: vdu.gcsiy -= (int32_t)vdu.gchar_spy; break;
    default: vdu.gcsiy += (int32_t)vdu.gchar_spy; break;
    }
    if (!was || (f & 0x40))
        return 0;
    return !in_window();
}

/* GCursorBdy. This moves to the boundary opposite the movement, n cells
 * in. */
static void cursor_bdy(uint32_t f, int32_t n)
{
    switch ((f & 0xE) >> 1) {
    case 0: case 2: vdu.gcsix = vdu.gwl + n * (int32_t)vdu.gchar_spx; break;
    case 1: case 3:
        vdu.gcsix = vdu.gwr - n * (int32_t)vdu.gchar_spx - ((int32_t)vdu.gchar_sx - 1);
        break;
    case 4: case 5: vdu.gcsiy = vdu.gwt - n * (int32_t)vdu.gchar_spy; break;
    default:
        vdu.gcsiy = vdu.gwb + n * (int32_t)vdu.gchar_spy + ((int32_t)vdu.gchar_sy - 1);
        break;
    }
}

static void ht(uint32_t f)
{
    if (cursor_move(f)) {
        cursor_bdy(f, 0);
        f ^= 8;
        if (cursor_move(f))
            cursor_bdy(f, 0);
    }
    vdu_ieg();
}

void vdu5_char(uint32_t c)
{
    paint(vdu.font[c - 32], vdu.fg_oe, c);
    if (!(vdu.cursor_flags & CF_NOMOVE))
        ht(vdu.cursor_flags);
}

void vdu5_delete(void)
{
    if (!(vdu.cursor_flags & CF_NOMOVE))
        ht(vdu.cursor_flags ^ 6);
    /* The unscaled cell is the hard font's block. The scaled cell is the
     * soft font's 127. */
    int fast = vdu.gchar_sx == 8 && (vdu.gchar_sy == 8 || vdu.gchar_sy == 16);
    paint(fast ? ros_vdu_hard_font[127 - 32] : vdu.font[127 - 32], vdu.bg_store, 127);
}

void vdu5_tab(uint32_t x, uint32_t y)
{
    uint32_t f = vdu.cursor_flags;
    cursor_bdy(f, (int32_t)x);
    cursor_bdy(f ^ 8, (int32_t)y);
    vdu_ieg();
}

void vdu5_control(uint8_t c)
{
    uint32_t f = vdu.cursor_flags;
    switch (c) {
    case 8: ht(f ^ 6); break;
    case 9: ht(f); break;
    case 10:
        f ^= 8;
        if (cursor_move(f))
            cursor_bdy(f, 0);
        vdu_ieg();
        break;
    case 11:
        f ^= 6 ^ 8;
        if (cursor_move(f))
            cursor_bdy(f, 0);
        vdu_ieg();
        break;
    case 12:
        vdu5_tab(0, 0);
        vdu_clg();
        break;
    case 13:
        cursor_bdy(f, 0);
        vdu_ieg();
        break;
    case 30:
        vdu5_tab(0, 0);
        break;
    default:
        break;
    }
}
