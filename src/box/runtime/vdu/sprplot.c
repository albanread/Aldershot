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
 * (Sources/Kernel: s.vdu.vdugrafg, s.vdu.vdugrafj, s.vdu.vdugrafk).
 */
/* sprplot.c -- sprites and the screen: PutSprite, PlotMask, GetSprite,
 * ScreenSave, ScreenLoad, PLOT &E8-&EF, VDU 23,27.
 *
 * This follows the kernel's Kernel/s/vdu/vdugrafg (PutSprite, PlotMask and
 * GenSpritePlotParmBlk), vdugrafj (GetSprite) and vdugrafk (ScreenSave and
 * ScreenLoad).
 *
 *   - The kernel's PutSprite plots words, not pixels. It takes the
 *     sprite's words, shifts them to the destination's bit position, and
 *     combines them with the destination through the GCOL action's (ora,
 *     eor) pair. The words are edge-masked and, with action 8 and up,
 *     masked. One sprite pixel is one destination pixel. The sprite's
 *     depth is never looked at, so a sprite of another depth plots as its
 *     bits. This is why SpriteExtend (52) is what translates.
 *   - A 1 bpp mask of a new-format sprite is expanded to the sprite's
 *     depth, with a mask bit becoming a whole pixel.
 *   - PlotMask fills the mask's shape with the background colour, action
 *     and ECF.
 *   - GetSprite takes the rectangle in the current destination's own
 *     words. What lies outside the graphics window is the background ECF,
 *     including the kernel's odd first word (see get_data).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "vduws.h"
#include "plot.h"
#include "sprite.h"

static uint32_t ror32(uint32_t v, uint32_t n)
{
    n &= 31;
    return n ? v >> n | v << (32 - n) : v;
}

static void internalise(int32_t x, int32_t y, int32_t *xi, int32_t *yi)
{
    *xi = (x + vdu.orgx) >> vdu.mv[MV_XEIG];
    *yi = (y + vdu.orgy) >> vdu.mv[MV_YEIG];
}

/* ---- GenSpritePlotParmBlk ------------------------------------------------------------------ */

struct plotblk {
    int32_t memw;               /* the first image word, from the image's start */
    int32_t row0, scrw;         /* the destination's top row (from the top), first word */
    int32_t rows1, cols1;
    uint32_t lmask, rmask, shftr, wpr;  /* wpr: words per sprite row */
};

/* Returns 0, or -1 if nothing is on screen. */
static int gen_parm(uint32_t sp, int32_t x, int32_t y, struct plotblk *p)
{
    uint32_t xs = 5 - vdu.mv[MV_LOG2BPC], np = plot_npix(), l2 = vdu.mv[MV_LOG2BPC];
    int32_t w1 = (int32_t)ros_ld32(sp + SP_WIDTH), h1 = (int32_t)ros_ld32(sp + SP_HEIGHT);
    int32_t below = vdu.gwb - y > 0 ? vdu.gwb - y : 0;
    int32_t top = y + h1, above = top - vdu.gwt > 0 ? top - vdu.gwt : 0;
    top -= above;
    int32_t red = below + above;
    if (VDU_CLIPPING) {        /* ClipSpritePlot */
        uint32_t bits = (uint32_t)(w1 + 1) * 32 + ros_ld32(sp + SP_LBIT) - (32 - ros_ld32(sp + SP_RBIT));
        int32_t cl = x > vdu.gwl ? x : vdu.gwl, cr = x + (int32_t)(bits >> l2);
        if (cr > vdu.gwr)
            cr = vdu.gwr;
        int32_t cb = top - (h1 - red);
        if (cr >= cl && top >= cb)
            vdu_cbox_merge(cl, cb, cr, top);
    }
    p->wpr = (uint32_t)w1 + 1;
    p->memw = above * (w1 + 1);
    p->row0 = (int32_t)vdu.mv[MV_YWIND] - top;
    int32_t wx = x >> xs, bitl = (int32_t)(((uint32_t)x & np) << l2);
    int32_t wr = vdu.gwr >> xs, br = (int32_t)((((uint32_t)vdu.gwr & np) + 1) << l2) - 1;
    int32_t wl = vdu.gwl >> xs, bl = (int32_t)(((uint32_t)vdu.gwl & np) << l2);
    int32_t shift = (int32_t)ros_ld32(sp + SP_LBIT) - bitl;
    int32_t rbit = (int32_t)ros_ld32(sp + SP_RBIT) - shift, skip = 0, lbit = bitl, first;
    if (rbit < 0)
        rbit += 32, skip += 4;
    if (rbit >= 32)
        rbit -= 32, skip -= 4;
    p->scrw = wx;
    int32_t d = wl - wx;
    if (d > 0) {
        p->scrw += d, p->memw += d, skip += 4 * d;
        first = wl, lbit = bl;
    } else if (d == 0) {
        first = wx;
        lbit = bitl > bl ? bitl : bl;
    } else {
        first = wx;
    }
    int32_t last = first + w1 - (skip >> 2), e = last - wr;
    if (e > 0)
        skip += 4 * e, rbit = br;
    else if (e == 0 && br < rbit)
        rbit = br;
    p->rows1 = h1 - red;
    p->cols1 = w1 - (skip >> 2);
    if (p->rows1 < 0 || p->cols1 < 0)
        return -1;
    p->rmask = ~(0xFFFFFFFEu << rbit);
    p->lmask = 0xFFFFFFFFu << lbit;
    if (p->cols1 == 0)
        p->lmask = p->rmask = p->lmask & p->rmask;
    if (shift < 0)
        shift += 32, p->memw -= 1;
    p->shftr = (uint32_t)shift;
    return 0;
}

/* The mask word that matches image word i, counted from the image's start
 * and spilling across rows as the kernel's reads do. Old and 1 bpp masks
 * are used as they are. Others are expanded from the 1 bpp mask to the
 * sprite's depth. */
struct maskinfo {
    uint32_t base, wpr, expand, l2, mwpr;
};

static uint32_t mask_word(const struct maskinfo *m, int32_t i)
{
    if (!m->expand)
        return ros_ld32(m->base + 4 * (uint32_t)i);
    int32_t row = i >= 0 ? i / (int32_t)m->wpr : -1;
    int32_t j = i - row * (int32_t)m->wpr;
    uint32_t ppw = 32u >> m->l2, bpp = 1u << m->l2, out = 0;
    uint32_t rowaddr = m->base + 4 * (uint32_t)row * m->mwpr, px = (uint32_t)j * ppw;
    for (uint32_t k = 0; k < ppw; k++) {
        uint32_t bit = (ros_ld32(rowaddr + 4 * ((px + k) >> 5)) >> ((px + k) & 31)) & 1;
        if (bit)
            out |= (bpp >= 32 ? ~0u : (1u << bpp) - 1) << (k * bpp);
    }
    return out;
}

/* The shifted source word k of row r. */
static uint32_t src_word(uint32_t base, const struct plotblk *p, int32_t r, int32_t k,
                         const struct maskinfo *m)
{
    int32_t i = p->memw + r * (int32_t)p->wpr + k;
    uint32_t a = m ? mask_word(m, i) : ros_ld32(base + 4 * (uint32_t)i);
    uint32_t b = 0;
    if (p->shftr)
        b = m ? mask_word(m, i + 1) : ros_ld32(base + 4 * (uint32_t)(i + 1));
    return a >> p->shftr | (p->shftr ? b << (32 - p->shftr) : 0);
}

static os_error *mask_setup(uint32_t sp, struct maskinfo *m)
{
    uint32_t mode = ros_ld32(sp + SP_MODE);
    if (mode & 0x80000000u)
        return spr_err(ERR_SPR_BADMODE);
    uint32_t t = spr_type(mode);
    m->base = sp + ros_ld32(sp + SP_TRANS);
    m->wpr = ros_ld32(sp + SP_WIDTH) + 1;
    m->expand = t >= 2;
    m->l2 = spr_type_bpp(t);
    if (m->expand) {
        uint32_t lb;
        spr_mask_width(sp, &m->mwpr, &lb);
    }
    return NULL;
}

static uint32_t *dest_word(const struct plotblk *p, int32_t r, int32_t k)
{
    return (uint32_t *)(vdu.screen + (uint32_t)(p->row0 + r) * vdu.mv[MV_LINELENGTH]) +
           p->scrw + k;
}

/* ---- PutSprite (28, 34) -------------------------------------------------------------------- */

static os_error *put(uint32_t sp, int32_t x, int32_t y, uint32_t a, int ignore_mask)
{
    struct plotblk p;
    if (!spr_geometry_ok(sp))
        return spr_err(ERR_SPR_BADFILE);
    a &= 15;
    int use_mask = !ignore_mask && a >= 8 && ros_ld32(sp + SP_TRANS) != ros_ld32(sp + SP_IMAGE);
    struct maskinfo mi;
    if (use_mask) {
        os_error *e = mask_setup(sp, &mi);
        if (e)
            return e;
    }
    if (!vdu.screen_ok || gen_parm(sp, x, y, &p))
        return NULL;
    uint32_t nib = ror32(0x970FEC53u, 4 * (a & 7)) >> 28;
    uint32_t zoo = nib & 8 ? ~0u : 0, zeo = nib & 4 ? ~0u : 0;
    uint32_t zoe = nib & 2 ? ~0u : 0, zee = nib & 1 ? ~0u : 0;
    uint32_t img = sp + ros_ld32(sp + SP_IMAGE);
    vdu_pre_wrch();
    for (int32_t r = 0; r <= p.rows1; r++)
        for (int32_t k = 0; k <= p.cols1; k++) {
            uint32_t s = src_word(img, &p, r, k, NULL);
            uint32_t e = (k == 0 ? p.lmask : ~0u) & (k == p.cols1 ? p.rmask : ~0u);
            if (use_mask)
                e &= src_word(0, &p, r, k, &mi);
            uint32_t ora = (s | zoo) ^ zeo, eor = (s | zoe) ^ zee, *d = dest_word(&p, r, k);
            *d = (*d | (ora & e)) ^ (eor & e);
        }
    vdu_post_wrch();
    return NULL;
}

os_error *spr_put(struct ros_cpu *s, uint32_t reason, uint32_t sp)
{
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC)
        return spr_err(ERR_SPR_NOTGRAPHICS);
    int32_t x = vdu.newptx, y = vdu.newpty;
    if (reason == 34)
        internalise((int32_t)s->r[3], (int32_t)s->r[4], &x, &y);
    return put(sp, x, y, s->r[5], 0);
}

/* ---- PlotMask (48, 49) --------------------------------------------------------------------- */

os_error *spr_plot_mask(struct ros_cpu *s, uint32_t reason, uint32_t sp)
{
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC)
        return spr_err(ERR_SPR_NOTGRAPHICS);
    int32_t x = vdu.newptx, y = vdu.newpty;
    if (reason == 49)
        internalise((int32_t)s->r[3], (int32_t)s->r[4], &x, &y);
    struct spr_mode d;
    os_error *e = spr_mode(ros_ld32(sp + SP_MODE), &d);
    if (e)
        return e;
    if (!spr_geometry_ok(sp))
        return spr_err(ERR_SPR_BADFILE);
    if (!vdu.screen_ok)
        return NULL;
    if (ros_ld32(sp + SP_TRANS) == ros_ld32(sp + SP_IMAGE)) {
        int32_t wpx = (int32_t)(((ros_ld32(sp + SP_WIDTH) + 1) * 32 - ros_ld32(sp + SP_LBIT) -
                                 31 + ros_ld32(sp + SP_RBIT)) >> d.l2bpc);
        if (VDU_CLIPPING) {    /* ClipPlotMask */
            int32_t xy[4] = { x, y, x + wpx - 1, y + (int32_t)ros_ld32(sp + SP_HEIGHT) };
            vdu_cbox_points(xy, 2);
        }
        const uint32_t *saved = plot_gcol;
        plot_gcol = vdu.bg_oe;
        vdu_pre_wrch();
        plot_rect(x, y, x + wpx - 1, y + (int32_t)ros_ld32(sp + SP_HEIGHT));
        vdu_post_wrch();
        plot_gcol = saved;
        return NULL;
    }
    struct maskinfo mi;
    if ((e = mask_setup(sp, &mi)) != NULL)
        return e;
    struct plotblk p;
    if (gen_parm(sp, x, y, &p))
        return NULL;
    vdu_pre_wrch();
    for (int32_t r = 0; r <= p.rows1; r++) {
        const uint32_t *t = vdu.bg_oe + 2 * ((uint32_t)(p.row0 + r) & 7);
        for (int32_t k = 0; k <= p.cols1; k++) {
            uint32_t m = src_word(0, &p, r, k, &mi) &
                         (k == 0 ? p.lmask : ~0u) & (k == p.cols1 ? p.rmask : ~0u);
            uint32_t *dw = dest_word(&p, r, k);
            *dw = (*dw | (t[0] & m)) ^ (t[1] & m);
        }
    }
    vdu_post_wrch();
    return NULL;
}

/* ---- GetSprite (14, 16) ---------------------------------------------------------------------- */

/* GetSpriteData, with the kernel's first-word quirk. The bits of the first
 * on-screen word that are outside the window come from r6. That is the
 * last background word written in this row or an earlier one, which is RM,
 * or nothing at first. */
static void get_data(uint32_t dst, const struct spr_get *g, uint32_t idx)
{
    int32_t words = g->width + 1, rows = g->height + 1;
    int32_t cw = words - g->lw_margin - g->rw_margin;
    int32_t rin = rows - g->top_margin - g->bot_margin;
    const uint32_t *bg = vdu.bg_ecf;
    /* With no screen memory at all (no GraphicsV driver), all of it is
     * treated as off the screen. The result is the background and never a
     * read of address 0 (#42). */
    if (cw == 0 || rin <= 0 || cw < 0 || !vdu.screen_ok) {
        for (int32_t r = 0; r < rows; r++, idx = (idx + 1) & 7)
            for (int32_t k = 0; k < words; k++, dst += 4)
                ros_st32(dst, bg[idx]);
        return;
    }
    uint32_t lm = 0xFFFFFFFFu << g->lb_margin, rm = ~(0xFFFFFFFEu << g->rb_margin), r6 = rm;
    int32_t colw = cw - 1;
    if (colw == 0)
        lm = rm = r6 = lm & rm;
    for (int32_t r = 0; r < g->top_margin; r++, idx = (idx + 1) & 7) {
        for (int32_t k = 0; k < words; k++, dst += 4)
            ros_st32(dst, bg[idx]);
        r6 = bg[idx];
    }
    uint32_t xs = 5 - vdu.mv[MV_LOG2BPC];
    const uint32_t *src = (const uint32_t *)(vdu.screen + (uint32_t)((int32_t)vdu.mv[MV_YWIND] -
                                                                      g->top_row) *
                                                              vdu.mv[MV_LINELENGTH]) +
                          (g->clip_l >> xs);
    for (int32_t r = 0; r < rin; r++, idx = (idx + 1) & 7) {
        const uint32_t *s = src;
        for (int32_t k = 0; k < g->lw_margin; k++, dst += 4)
            ros_st32(dst, bg[idx]), r6 = bg[idx];
        uint32_t w = *s++;
        r6 &= ~lm;
        ros_st32(dst, (w & lm) | r6), dst += 4;
        if (colw > 0) {
            for (int32_t k = 0; k < colw - 1; k++, dst += 4)
                ros_st32(dst, *s++);
            uint32_t b = bg[idx] & ~rm;
            w = *s++;
            ros_st32(dst, (w & rm) | b), dst += 4;
            r6 = b;
        }
        for (int32_t k = 0; k < g->rw_margin; k++, dst += 4)
            ros_st32(dst, bg[idx]), r6 = bg[idx];
        src = (const uint32_t *)((const uint8_t *)src + vdu.mv[MV_LINELENGTH]);
    }
    for (int32_t r = 0; r < g->bot_margin; r++, idx = (idx + 1) & 7)
        for (int32_t k = 0; k < words; k++, dst += 4)
            ros_st32(dst, bg[idx]);
}

os_error *spr_get_sprite(struct ros_cpu *s, uint32_t reason, uint32_t area)
{
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC)
        return spr_err(ERR_SPR_NOTGRAPHICS);
    vdu.sp_choose = 0;
    int32_t x0, y0, x1, y1;
    if (reason == 14) {
        x0 = vdu.oldx, y0 = vdu.oldy, x1 = vdu.gcsix, y1 = vdu.gcsiy;
    } else {
        internalise((int32_t)s->r[4], (int32_t)s->r[5], &x0, &y0);
        internalise((int32_t)s->r[6], (int32_t)s->r[7], &x1, &y1);
    }
    int32_t l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0;
    int32_t b = y0 < y1 ? y0 : y1, t = y0 < y1 ? y1 : y0;
    uint32_t idx = plot_erow(t);
    struct spr_get g = { .mode = vdu.mode_no };
    os_error *e = spr_sanitize_mode(&g.mode);
    if (e)
        return e;
    uint8_t key[12];
    spr_get_name(s->r[2], key);
    uint32_t sp = spr_find_key(area, key);
    vdu_pre_wrch();
    if (sp) {
        if (sp == vdu.dest_sprite) {
            vdu_post_wrch();
            return spr_err(ERR_SPR_ISDEST);
        }
        if ((e = spr_pre_header(&g, s->r[3], l, b, r, t)) != NULL)
            goto out;
        int32_t dw = ((int32_t)g.size - (int32_t)ros_ld32(sp + SP_NEXT)) / 4;
        if (dw < 0)
            spr_remove_words(area, sp, (uint32_t)-dw,
                             ros_ld32(sp + SP_NEXT) - ros_ld32(sp + SP_IMAGE) - 4 * (uint32_t)-dw);
        else if (dw > 0 && (e = spr_extend(area, sp, (uint32_t)dw)) != NULL)
            goto out;
        spr_write_header(sp, &g, key);
        get_data(sp + g.image, &g, idx);
    } else {
        if ((e = spr_pre_header(&g, s->r[3], l, b, r, t)) != NULL ||
            (e = spr_create_header(area, &g, key, &sp)) != NULL)
            goto out;
        get_data(sp + g.image, &g, idx);
        ros_st32(area + SA_NUMBER, ros_ld32(area + SA_NUMBER) + 1);
        ros_st32(area + SA_FREE, ros_ld32(area + SA_FREE) + g.size);
    }
    if (ros_ld32(sp + SP_MODE) >= 256)
        spr_remove_lh_wastage(area, sp);
    if (s->r[0] < 0x100) {
        vdu.sp_choose = sp;
        memcpy(vdu.sp_choose_name, ros_ptr(sp + SP_NAME), 12);
    } else {
        s->r[2] = sp;
    }
out:
    vdu_post_wrch();
    return e;
}

/* ---- ScreenSave (2) and ScreenLoad (3) --------------------------------------------------------- */

static os_error *swi_call(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

os_error *spr_screen_save(struct ros_cpu *s)
{
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC)
        return spr_err(ERR_SPR_NOTGRAPHICS);
    struct spr_get g = { .mode = vdu.mode_no };
    os_error *e = spr_sanitize_mode(&g.mode);
    if (e)
        return e;
    uint32_t pal = vdu.mv[MV_LOG2BPP] >= 4 ? 0 : s->r[3];
    if ((e = spr_pre_header(&g, pal, vdu.gwl, vdu.gwb, vdu.gwr, vdu.gwt)) != NULL)
        return e;
    uint32_t words = (uint32_t)g.width + 1, rows = (uint32_t)g.height + 1;
    uint8_t *file = ros_rma_alloc(12 + g.size);
    if (!file)
        return spr_err(ERR_SPR_NOROOM);
    uint32_t f = ros_addr(file), sp = f + 12;
    uint8_t key[12] = "screendump";
    spr_write_header(sp, &g, key);
    uint32_t s_l = 0, narrower = 0;
    if (g.mode >= 256 && g.lbit) {
        s_l = g.lbit;
        uint32_t bits = g.rbit - s_l + (uint32_t)g.width * 32, nw = bits >> 5;
        ros_st32(sp + SP_LBIT, 0);
        ros_st32(sp + SP_RBIT, bits & 31);
        if (nw != (uint32_t)g.width) {
            ros_st32(sp + SP_NEXT, g.size - 4 * rows);
            narrower = 1;
        }
        ros_st32(sp + SP_WIDTH, nw);
    }
    uint32_t next = ros_ld32(sp + SP_NEXT);
    ros_st32(f, 1), ros_st32(f + 4, 16), ros_st32(f + 8, 16 + next);
    vdu_pre_wrch();
    uint32_t xs = 5 - vdu.mv[MV_LOG2BPC];
    const uint8_t *row = vdu.screen + (uint32_t)((int32_t)vdu.mv[MV_YWIND] - vdu.gwt) *
                                          vdu.mv[MV_LINELENGTH] + 4 * (uint32_t)(vdu.gwl >> xs);
    uint32_t out = sp + g.image, outw = narrower ? words - 1 : words;
    for (uint32_t r = 0; r < rows; r++, row += vdu.mv[MV_LINELENGTH]) {
        const uint32_t *src = (const uint32_t *)row;
        for (uint32_t j = 0; j < outw; j++, out += 4) {
            uint32_t v = src[j];
            if (s_l)
                v = v >> s_l | (j + 1 < words ? src[j + 1] << (32 - s_l) : 0);
            ros_st32(out, v);
        }
    }
    vdu_post_wrch();
    uint32_t r[10] = { 10, s->r[2], 0xFF9, 0, f, f + 12 + next };
    e = swi_call(XOS_File, r);
    ros_rma_free(file);
    return e;
}

/* WritePaletteFromSprite. This sets the mode that the sprite needs, then
 * its palette. */
static os_error *palette_from_sprite(uint32_t sp)
{
    uint32_t m = ros_ld32(sp + SP_MODE), mv[MV_COUNT];
    if (vdu_mode_vars(m, mv))
        return spr_err(ERR_SPR_BADMODE);
    uint32_t xres = (ros_ld32(sp + SP_WIDTH) * 32 + ros_ld32(sp + SP_RBIT) + 1) >> mv[MV_LOG2BPP];
    uint32_t yres = ros_ld32(sp + SP_HEIGHT) + 1, flags = mv[MV_FLAGS], nc = mv[MV_NCOLOUR];
    int32_t pal = (int32_t)ros_ld32(sp + SP_IMAGE) - 44;
    if (mv[MV_LOG2BPP] == 3) {
        flags = pal == 2048 ? MF_FULLPALETTE : 0;
        nc = pal == 2048 ? 255 : 63;
    }
    uint32_t fm = MF_FULLPALETTE | MF_FORMAT;
    if (vdu.mv[MV_XWIND] + 1 != xres || vdu.mv[MV_YWIND] != yres - 1 ||
        vdu.mv[MV_LOG2BPP] != mv[MV_LOG2BPP] || vdu.mv[MV_XEIG] != mv[MV_XEIG] ||
        vdu.mv[MV_YEIG] != mv[MV_YEIG] || vdu.mv[MV_NCOLOUR] != nc ||
        ((vdu.mv[MV_FLAGS] ^ flags) & fm)) {
        uint32_t *sel = ros_rma_alloc(64);
        uint32_t w[] = { 1, xres, yres, mv[MV_LOG2BPP], 0xFFFFFFFFu, 4, mv[MV_XEIG], 5,
                         mv[MV_YEIG], 0, flags, 3, nc, 0xFFFFFFFFu };
        memcpy(sel, w, sizeof w);
        uint32_t r[10] = { 0, ros_addr(sel) };
        os_error *e = swi_call(XOS_ScreenMode, r);
        if (e && m < 256) {
            uint32_t r2[10] = { 0, m };
            e = swi_call(XOS_ScreenMode, r2);
        }
        ros_rma_free(sel);
        if (e)
            return e;
    }
    if (ros_ld32(sp + SP_IMAGE) != 44 && vdu.mv[MV_NCOLOUR] <= 255) {
        uint8_t *b = ros_rma_alloc(8);
        for (uint32_t i = vdu.mv[MV_NCOLOUR] + 1; i-- > 0;) {
            uint32_t f1 = ros_ld32(sp + 44 + 8 * i), f2 = ros_ld32(sp + 44 + 8 * i + 4);
            uint32_t words[2] = { f1, f2 }, types[2] = { f1 == f2 ? 16u : 17u, 18u };
            for (int k = 0; k < (f1 == f2 ? 1 : 2); k++) {
                uint32_t c = words[k];
                b[0] = (uint8_t)i, b[1] = (uint8_t)((c & 0x80) | types[k]);
                b[2] = (uint8_t)(c >> 8), b[3] = (uint8_t)(c >> 16), b[4] = (uint8_t)(c >> 24);
                uint32_t r[10] = { 12, ros_addr(b) };
                swi_call(XOS_Word, r);
            }
        }
        ros_rma_free(b);
    }
    return NULL;
}

os_error *spr_screen_load(struct ros_cpu *s)
{
    /* The file is opened first, as the kernel opens it (open_read,
     * open_mustopen and open_nodir). A file that is not there gives
     * OS_Find's error, which names it in full. */
    uint32_t o[10] = { 0x4C, s->r[2] };
    os_error *e = swi_call(XOS_Find, o);
    if (e)
        return e;
    uint32_t c[10] = { 0, o[0] };
    swi_call(XOS_Find, c);
    uint32_t r[10] = { 5, s->r[2] };
    if ((e = swi_call(XOS_File, r)) != NULL)
        return e;
    if (r[0] != 1) {
        uint32_t m[10] = { 19, s->r[2], r[0] };
        e = swi_call(XOS_File, m);
        return e ? e : spr_err(ERR_SPR_BADFILE);
    }
    uint32_t len = r[4];
    uint8_t *buf = ros_rma_alloc(len + 16);
    if (!buf)
        return spr_err(ERR_SPR_NOTENOUGHROOM);
    uint32_t f = ros_addr(buf);
    uint32_t l[10] = { 255, s->r[2], f + 4, 0 };
    if ((e = swi_call(XOS_File, l)) != NULL)
        goto out;
    uint32_t sp = f + ros_ld32(f + 8);          /* the first sprite, at saFirst from the area's start */
    if ((e = palette_from_sprite(sp)) != NULL)
        goto out;
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC) {
        e = spr_err(ERR_SPR_NOTGRAPHICS);
        goto out;
    }
    e = put(sp, vdu.gwl, vdu.gwb, 0, 1);
out:
    ros_rma_free(buf);
    return e;
}

/* ---- PLOT &E8-&EF and VDU 23,27 ------------------------------------------------------------------ */

static void chosen(void)
{
    if (vdu.sp_choose || !vdu.sp_area)
        return;
    uint32_t sp = spr_find_key(vdu.sp_area, vdu.sp_choose_name);
    if (sp)
        vdu.sp_choose = sp;
}

void vdu_plot_sprite(uint32_t k)
{
    chosen();
    if (!vdu.sp_choose)
        return;
    static const uint32_t action[4] = { 5, 0, 4, 0 };
    uint32_t r[10] = { 0x200 + 28, vdu.sp_area, vdu.sp_choose, 0, 0,
                       (k & 3) == 1 ? vdu.gplfmd & 15 : action[k & 3] };
    if ((k & 3) == 3)
        r[0] = 0x200 + 48;
    swi_call(XOS_SpriteOp, r);
}

void vdu_vdu23_27(uint32_t op, uint32_t n)
{
    if (op > 1)
        return;
    uint8_t *name = ros_rma_alloc(8);
    snprintf((char *)name, 8, "%u", n);
    uint32_t r[10] = { op == 0 ? 24u : 14u, 0, ros_addr(name), 0 };
    swi_call(XOS_SpriteOp, r);
    ros_rma_free(name);
}
