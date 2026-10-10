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
 * (Sources/Kernel: s.vdu.vdugrafl).
 */
/* sprout.c -- output to a sprite or its mask (OS_SpriteOp 60, 61), and
 * back to the screen.
 *
 * This follows the kernel's Kernel/s/vdu/vdugrafl (SwitchOutputToSprite,
 * PackVars, UnpackVars and ValidateVars).
 *
 *   - R1 and R2 give the sprite. R2 = 0 means the screen. R3 is a save
 *     area: 0 for none, 1 for the kernel's own, or else an address. R0-R3
 *     come back as the previous destination, so passing them back restores
 *     it.
 *   - The outgoing destination's VDU state is packed into its save area.
 *     The incoming one's is unpacked from its save area if that says
 *     "VOTS", or else set to defaults, VDU 4 and all.
 *   - The destination's variables are the sprite's mode's, and its size is
 *     the sprite's. LineLength is its words and XWindLimit is its pixels
 *     less one, and so on. Text in a sprite always scrolls by copying. The
 *     text cursor is off.
 *   - A save area is 384 bytes, as OS_SpriteOp 62 says. It holds "VOTS",
 *     three OS_Byte variables, and 376 bytes of VDU state in the kernel's
 *     order (its CompressionTable). Software only sees its size and its
 *     flag.
 *
 * The palette, the cursor's flashing and GraphicsV stay the display's.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "vduws.h"
#include "plot.h"
#include "sprite.h"

#define VOTS 0x53544F56u
#define SAVE_SIZE 384
#define SERVICE_SWITCHING_OUTPUT 0x72u

static uint8_t mos_area[SAVE_SIZE];     /* VduSaveArea: save area 1 */

uint8_t *vdu_mos_area(void)
{
    return mos_area;
}

static uint8_t *save_ptr(uint32_t a)
{
    return a == 1 ? mos_area : a ? ros_ptr(a) : NULL;
}

/* ---- packing ------------------------------------------------------------------------------ */

struct cur {
    uint8_t *p;
    int pack;
};

static void w32(struct cur *c, void *v)
{
    if (c->pack)
        memcpy(c->p, v, 4);
    else
        memcpy(v, c->p, 4);
    c->p += 4;
}

static void wbytes(struct cur *c, void *v, size_t n)
{
    if (c->pack)
        memcpy(c->p, v, n);
    else
        memcpy(v, c->p, n);
    c->p += n;
}

static uint32_t os_byte(uint32_t n, uint32_t r1, uint32_t r2)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = n, s.r[1] = r1, s.r[2] = r2;
    ros_swi(&s, XOS_Byte);
    return s.r[1];
}

/* The kernel's CompressionTable, in its order: 376 bytes. */
static void vars(uint8_t *area, int pack)
{
    struct cur c = { area + 8, pack };
    uint32_t zero = 0, q[7] = { 0 };
    w32(&c, &vdu.gplfmd), w32(&c, &vdu.gplbmd), w32(&c, &vdu.gfcol), w32(&c, &vdu.gbcol);
    w32(&c, &vdu.gwl), w32(&c, &vdu.gwb), w32(&c, &vdu.gwr), w32(&c, &vdu.gwt);
    if (pack) {                                 /* the queue is 28 bytes */
        memcpy(q, ros_ptr(vdu.qq), 16);
        q[4] = vdu.qcode, q[5] = (uint32_t)vdu.qwant, q[6] = (uint32_t)vdu.qlen;
    }
    wbytes(&c, q, 28);
    if (!pack) {
        memcpy(ros_ptr(vdu.qq), q, 16);
        vdu.qcode = (uint8_t)q[4], vdu.qwant = (int)q[5], vdu.qlen = (int)q[6];
    }
    w32(&c, &vdu.twl), w32(&c, &vdu.twb), w32(&c, &vdu.twr), w32(&c, &vdu.twt);
    w32(&c, &vdu.orgx), w32(&c, &vdu.orgy), w32(&c, &vdu.gcsx), w32(&c, &vdu.gcsy);
    w32(&c, &vdu.olderx), w32(&c, &vdu.oldery), w32(&c, &vdu.oldx), w32(&c, &vdu.oldy);
    w32(&c, &vdu.gcsix), w32(&c, &vdu.gcsiy), w32(&c, &vdu.newptx), w32(&c, &vdu.newpty);
    w32(&c, &vdu.tfore), w32(&c, &vdu.tback), w32(&c, &vdu.cx), w32(&c, &vdu.cy);
    w32(&c, &zero), w32(&c, &zero);             /* InputCursorX/Y: no cursor editing */
    w32(&c, &vdu.status);
    w32(&c, &vdu.cur_desired), w32(&c, &vdu.cur_start), w32(&c, &vdu.cur_end);
    w32(&c, &vdu.cur_counter), w32(&c, &vdu.cur_speed), w32(&c, &vdu.reg10copy);
    wbytes(&c, vdu.ecf, 32);
    wbytes(&c, &vdu.dot_style, 8);
    w32(&c, &vdu.tftint), w32(&c, &vdu.tbtint), w32(&c, &vdu.gftint), w32(&c, &vdu.gbtint);
    w32(&c, &vdu.cursor_flags), w32(&c, &vdu.cur_stack);
    w32(&c, &vdu.ecf_shift), w32(&c, &vdu.ecf_yoffset);
    w32(&c, &vdu.gchar_sx), w32(&c, &vdu.gchar_sy), w32(&c, &vdu.gchar_spx), w32(&c, &vdu.gchar_spy);
    w32(&c, &vdu.dot_cnt);
    wbytes(&c, &vdu.dot_pat, 8);
    w32(&c, &vdu.dot_length), w32(&c, &vdu.bbc_ecfs);
    wbytes(&c, vdu_cbox(), 20);                 /* ClipBoxEnable, ClipBoxLCol..TRow */
    wbytes(&c, vdu.fg_pattern, 32), wbytes(&c, vdu.bg_pattern, 32);
    w32(&c, &vdu.text_fg), w32(&c, &vdu.text_bg);
}

static void pack(uint32_t a)
{
    uint8_t *p = save_ptr(a);
    if (!p)
        return;
    uint32_t flag = VOTS;
    memcpy(p, &flag, 4);
    p[4] = (uint8_t)os_byte(199, 0, 255);       /* the *Spool handle */
    p[5] = (uint8_t)os_byte(236, 0, 255);       /* WrchDest */
    p[6] = (uint8_t)(-vdu.qwant);
    vars(p, 1);
}

static void unpack(uint8_t *p)
{
    os_byte(199, p[4], 0);
    os_byte(236, p[5], 0);
    vars(p, 0);
}

/* ---- the destination ------------------------------------------------------------------------ */

/* Resets output to the screen on a mode change, with the kernel's own save
 * area empty. */
os_error *vdu_output_to_screen(void)
{
    vdu.dest_select = 0x23C;
    vdu.dest_area = vdu.dest_sprite = 0;
    vdu.save_area = 1;
    memset(mos_area, 0, 4);
    return NULL;
}

/* ValidateVars. The text window is applied again as VDU 28 would. The
 * graphics window is kept only if it fits. */
static void validate(void)
{
    int32_t l = vdu.twl, b = vdu.twb, r = vdu.twr, t = vdu.twt;
    vdu.twl = vdu.twt = 0;
    vdu.twr = (int32_t)vdu.mv[MV_SCRRCOL], vdu.twb = (int32_t)vdu.mv[MV_SCRBROW];
    if (!(l > r || r > (int32_t)vdu.mv[MV_SCRRCOL] || t > b || b > (int32_t)vdu.mv[MV_SCRBROW]))
        vdu.twl = l, vdu.twb = b, vdu.twr = r, vdu.twt = t;
    if ((uint32_t)vdu.gwr > vdu.mv[MV_XWIND] || (uint32_t)vdu.gwt > vdu.mv[MV_YWIND])
        vdu.gwl = vdu.gwb = 0, vdu.gwr = (int32_t)vdu.mv[MV_XWIND],
        vdu.gwt = (int32_t)vdu.mv[MV_YWIND];
    /* The save area is the program's memory, so it can hold anything. The
     * rest of the state is used to address the screen and the queue's
     * block, and is made safe to use. */
    if (vdu.gwl < 0 || vdu.gwb < 0 || vdu.gwl > vdu.gwr || vdu.gwb > vdu.gwt)
        vdu.gwl = vdu.gwb = 0, vdu.gwr = (int32_t)vdu.mv[MV_XWIND],
        vdu.gwt = (int32_t)vdu.mv[MV_YWIND];
    if (vdu.cx < vdu.twl || vdu.cx > vdu.twr || vdu.cy < vdu.twt || vdu.cy > vdu.twb)
        vdu.cx = vdu.twl, vdu.cy = vdu.twt;
    if (vdu.cur_end > vdu.row_mult)
        vdu.cur_end = vdu.row_mult;
    if (vdu.cur_start > vdu.cur_end)
        vdu.cur_start = vdu.cur_end;
    if (vdu.qlen < 0 || vdu.qlen > 15 || vdu.qwant < 0 || vdu.qwant > 255)
        vdu.qwant = vdu.qlen = 0;
    if ((vdu.cursor_flags & CF_TELETEXT) != (vdu.mv[MV_FLAGS] & MF_TELETEXT ? CF_TELETEXT : 0u))
        vdu.cursor_flags ^= CF_TELETEXT;
    vdu.cursor_flags &= ~CF_SPLIT;
}

static int greyscale_palette(uint32_t sp, int32_t pal)
{
    for (int32_t i = 0; i < pal; i += 4) {
        uint32_t w = ros_ld32(sp + 44 + (uint32_t)i);
        uint8_t r = (uint8_t)(w >> 8), g = (uint8_t)(w >> 16), b = (uint8_t)(w >> 24);
        if (r != g || g != b)
            return 0;
    }
    return 1;
}

os_error *spr_switch_output(struct ros_cpu *s, uint32_t reason, uint32_t area, uint32_t sp)
{
    uint32_t save = s->r[3];
    uint8_t *sa = save_ptr(save);
    uint32_t flag = 0;
    if (sa)
        memcpy(&flag, sa, 4);
    if (flag != 0 && flag != VOTS)
        return spr_err(ERR_SPR_BADSAVEAREA);
    if (sp && !spr_geometry_ok(sp))
        return spr_err(ERR_SPR_BADFILE);
    uint32_t prev[4] = { vdu.dest_select, vdu.dest_area, vdu.dest_sprite, vdu.save_area };
    vdu.dest_select = 0x200 | reason;
    vdu.dest_area = sp ? area : 0;
    vdu.dest_sprite = sp;
    vdu_pre_wrch();
    if (vdu.save_area)
        pack(vdu.save_area);
    vdu.save_area = save;

    uint32_t mv[MV_COUNT], screensize = vdu.mv[MV_SCREENSIZE];
    if (!sp) {
        memcpy(mv, vdu.dmv, sizeof mv);
        vdu.screen_start = vdu.dscreen_start;
        vdu.mode_no = vdu.dmode_no;
        vdu.screen_ok = vdu.dscreen_start != 0;
    } else {
        spr_remove_lh_wastage(area, sp);
        uint32_t w = ros_ld32(sp + SP_WIDTH), h = ros_ld32(sp + SP_HEIGHT);
        uint32_t rbit = ros_ld32(sp + SP_RBIT), image = ros_ld32(sp + SP_IMAGE);
        uint32_t mode = ros_ld32(sp + SP_MODE), orig = mode;
        if (reason == 61) {
            image = ros_ld32(sp + SP_TRANS);
            if (mode >> 27) {
                uint32_t words;
                spr_mask_width(sp, &words, &rbit);
                w = words - 1;
                uint32_t alpha = mode & 0x80000000u ? 4 : 1;
                if (((mode >> 27) & 15) == 15)
                    mode = (mode & 0xF0) | 1 | 15u << 27 | alpha << 20;
                else
                    mode = (mode & 0x07FFFFFFu) | alpha << 27;
            }
        }
        os_error *e = vdu_mode_vars(mode, mv);
        if (e) {
            /* An unreadable mode must not leave the destination switched.
             * Otherwise the VDU would go on drawing into this sprite. The
             * kernel fails the switch before any of it moves. */
            vdu.dest_select = prev[0];
            vdu.dest_area = prev[1];
            vdu.dest_sprite = prev[2];
            vdu.save_area = prev[3];
            vdu_post_wrch();
            return e;
        }
        uint32_t px = ((w + 1) * 32 + rbit - 31) >> mv[MV_LOG2BPC];
        mv[MV_LINELENGTH] = (w + 1) * 4;
        mv[MV_XWIND] = px - 1;
        mv[MV_SCRRCOL] = (px >> 3) - 1;
        mv[MV_YWIND] = h;
        mv[MV_SCRBROW] = ((h + 1) >> 3) - 1;
        mv[MV_SCREENSIZE] = screensize;
        mv[MV_FLAGS] |= MF_HARDSCROLLOFF;
        int32_t a = (int32_t)ros_ld32(sp + SP_IMAGE), b = (int32_t)ros_ld32(sp + SP_TRANS);
        int32_t pal = (a < b ? a : b) - 44;
        if (mv[MV_LOG2BPP] == 3 && pal == 2048)
            mv[MV_FLAGS] |= MF_FULLPALETTE, mv[MV_NCOLOUR] = 255;
        if (reason == 61 && (orig & 0x80000000u))
            mv[MV_FLAGS] |= MF_GREYSCALE | MF_FULLPALETTE, mv[MV_NCOLOUR] = 255;
        if (reason == 60 &&
            (mv[MV_LOG2BPP] < 3 || (mv[MV_LOG2BPP] == 3 && (mv[MV_FLAGS] & MF_FULLPALETTE))) &&
            pal != 0 && greyscale_palette(sp, pal))
            mv[MV_FLAGS] |= MF_GREYSCALE;
        vdu.screen_start = sp + image;
        vdu.mode_no = mode;
        vdu.screen_ok = 1;
    }
    memcpy(vdu.mv, mv, sizeof mv);
    vdu.screen = ros_ptr(vdu.screen_start);
    vdu_derive();
    int32_t xe = (int32_t)mv[MV_XEIG], ye = (int32_t)mv[MV_YEIG];
    vdu.aspect = xe > ye ? 1 : xe < ye ? 2 : 0;

    sa = save_ptr(save);
    if (sa && flag == VOTS) {
        unpack(sa);
        validate();
        vdu_set_colour();
        vdu.cursor_flags |= CF_TEUPDATE;        /* the text colours spread when next used */
    } else {
        vdu.qwant = 0;
        vdu.gchar_sx = vdu.gchar_sy = vdu.gchar_spx = vdu.gchar_spy = 8;
        vdu_cbox()[0] = 0;                      /* ChangedBox off */
        vdu.cursor_flags &= ~(CF_ACTUAL | CF_VDU5 | CF_SPLIT | CF_PAGEMODE | CF_TELETEXT |
                              CF_CLIPBOX | CF_C81);
        if (!sp && (mv[MV_FLAGS] & MF_TELETEXT)) {     /* back to a teletext screen */
            vdu.cursor_flags |= CF_TELETEXT;            /* TeletextInit, with the map kept */
            vdu_ttx_init(0);
        }
        vdu_cursor_init();
        vdu_default_colours();
        vdu_plot_mode();
        vdu_default_windows();
    }
    vdu.cur_stack = 0;
    vdu.cursor_flags &= ~CF_ACTUAL;
    vdu_post_wrch();

    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XColourTrans_InvalidateCache);  /* as the kernel tells it, ignoring errors */
    ros_cpu_enter(&c);
    c.r[1] = SERVICE_SWITCHING_OUTPUT;
    c.r[2] = vdu.dest_select, c.r[3] = vdu.dest_area, c.r[4] = vdu.dest_sprite;
    c.r[5] = vdu.save_area;
    ros_service_call(&c);
    s->r[0] = prev[0], s->r[1] = prev[1], s->r[2] = prev[2], s->r[3] = prev[3];
    return NULL;
}
