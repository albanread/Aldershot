/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2010 Castle Technology Ltd
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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/Video/Render/SprExtend: Sources/SprExtend, Sources/SprOp,
 * Sources/SWIs, Sources/SprAdjSize, Sources/SprTrans, Sources/MsgCode).
 */

/* spriteextend.c -- SpriteExtend, reimplemented as a native module.
 *
 * On RISC OS 5 SpriteExtend (Video/Render/SprExtend) claims SpriteV and
 * does most of the plotting: scaled and transformed sprites, colour
 * translation, and some area operations.  Its heart builds ARM code at run
 * time, which nothing in ROSGD could run.  So it is written again here from
 * its sources, pixel for pixel, and checked against the 32-bit system on
 * the farm (tests/vdu).  Like the original it works only through SpriteV
 * and the kernel's public calls (OS_ReadVduVariables, OS_ReadModeVariable,
 * and OS_SpriteOp for what it hands back).  A host renderer can therefore
 * take its place on the vector later.
 *
 * This file holds the module, the claim, SpriteExtend's own sprite lookup
 * and errors, and the operations on areas.  These are 17 CheckSpriteArea,
 * 35 AppendSprite, 36 SetPointerShape, 37 CreateRemovePalette, 38
 * CreateRemoveAlpha, 57 and 58 InsertDeleteRows and Columns, and 53, which
 * RISC OS 5 refuses.  Plotting, scaled and transformed, is in scaled.c.
 *
 * The JPEG SWIs are in jpeg.c.  Blending onto 256 colours or fewer needs
 * the BlendTable and InverseTable modules and is not done yet.  Those
 * plots say so.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "sprext.h"

#define SPRITEV 0x1Fu

/* ---- errors ---------------------------------------------------------------------- */

os_error *sx_err(uint32_t n)
{
    switch (n) {
    case SX_BADADDRESS: return ros_error(n, "Bad address");
    case SX_NOWORK: return ros_error(n, "SpriteExtend: Not enough memory in system sprite area");
    case SX_NOTENOUGHROOM: return ros_error(n, "SpriteExtend: Not enough memory for sprite operation");
    case SX_DOESNTEXIST: return ros_error(n, "SpriteExtend: Sprite doesn't exist");
    case SX_DIVZERO: return ros_error(n, "SpriteExtend: Division by zero");
    case SX_INVRC: return ros_error(n, "SpriteExtend: Invalid row or column");
    case SX_NOIROOM:
        return ros_error(n, "SpriteExtend: Not enough memory to insert sprite row or column");
    case SX_BADMODE: return ros_error(n, "SpriteExtend: Invalid sprite mode");
    case SX_BADTRAN: return ros_error(n, "SpriteExtend: Bad colour translation table");
    case SX_APPERR: return ros_error(n, "SpriteExtend: Can't append sprite");
    case SX_BADFLAGS: return ros_error(n, "SpriteExtend: Attempt to set reserved flags");
    case SX_BADDEPTH:
        return ros_error(n, "Mask or palette operations not supported in this display depth");
    case SX_BADDATA: return ros_error(n, "Unrecognised sprite data");
    case SX_NOGRSCL:
        return ros_error(n, "Operation 'PutSpriteGreyScaled' is not supported by this version "
                            "of the SpriteExtend module");
    default: return ros_error(n, "SpriteExtend error");
    }
}

uint32_t sx_swi(uint32_t n, uint32_t r[10], os_error **e)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 10 * sizeof r[0]);
    *e = c.v ? ros_ptr(c.r[0]) : NULL;
    return c.c;
}

/* ---- the state it reads ------------------------------------------------------------- */

void sx_read_dest(struct sx_dest *d)
{
    static const uint32_t vars[] = { 4, 5, 9, 10, 136, 137, 128, 129, 130, 131, 6, 148, 12, 0, 3,
                                     171, 0xFFFFFFFFu };
    uint32_t *in = ros_ptr(ros_vdu_scratch()), *out = in + 32;
    memcpy(in, vars, sizeof vars);
    uint32_t r[10] = { ros_addr(in), ros_addr(out) };
    os_error *e;
    sx_swi(XOS_ReadVduVariables, r, &e);
    d->xeig = (int32_t)out[0], d->yeig = (int32_t)out[1];
    d->l2bpp = out[2], d->l2bpc = out[3];
    d->orgx = (int32_t)out[4], d->orgy = (int32_t)out[5];
    d->gwl = (int32_t)out[6], d->gwb = (int32_t)out[7], d->gwr = (int32_t)out[8];
    d->gwt = (int32_t)out[9];
    d->linelen = out[10], d->screen = out[11], d->ywind = out[12];
    d->flags = out[13], d->ncolour = out[14];
    d->fg_ecf = out[15], d->bg_ecf = out[15] + 64;
    d->bpp = 1u << d->l2bpp, d->bpc = 1u << d->l2bpc;
    ros_vdu_ecf_offsets(&d->ecf_shift, &d->ecf_yoffset);
}

/* readspritevars: 0, or -1 if the kernel does not know the mode */
int sx_read_mode(uint32_t mode, struct sx_mode *m)
{
    static const uint32_t which[6] = { 10, 9, 4, 5, 0, 3 };
    uint32_t v[6];
    for (int i = 0; i < 6; i++) {
        uint32_t r[10] = { mode, which[i] };
        os_error *e;
        if (sx_swi(XOS_ReadModeVariable, r, &e))
            return -1;
        v[i] = r[2];
    }
    m->l2bpc = v[0], m->l2bpp = v[1], m->xeig = v[2], m->yeig = v[3], m->flags = v[4];
    m->ncolour = v[5];
    return 0;
}

/* ---- finding the sprite (getspritename, getspriteaddr) -------------------------------- */

static os_error *find_named(uint32_t code, uint32_t area_in, uint32_t reg, uint32_t *area,
                            uint32_t *sp)
{
    uint32_t hi = code & ~0xFFu;
    uint8_t key[12] = { 0 };
    if (hi != 0x200) {
        if (reg < 256)
            return sx_err(SX_BADADDRESS);
        for (int i = 0; i < 12; i++) {
            uint8_t c = (uint8_t)ros_ld8(reg + (uint32_t)i);
            if (c <= 0x20)
                break;
            if (c >= 'A' && c <= 'Z')
                c += 32;
            key[i] = c;
        }
    }
    uint32_t a;
    if (hi == 0) {
        uint32_t r[10] = { 3 };
        os_error *e;
        sx_swi(XOS_ReadDynamicArea, r, &e);
        if (e || r[1] == 0)
            return sx_err(SX_NOWORK);
        a = r[0];
    } else {
        a = area_in;
        if ((a & 3) || a < 256)
            return sx_err(SX_BADADDRESS);
    }
    *area = a;
    if (hi != 0x200) {
        uint32_t p = a + ros_ld32(a + SA_FIRST), end = a + ros_ld32(a + SA_FREE);
        for (;;) {
            if (p >= end)
                return sx_err(SX_DOESNTEXIST);
            if (!memcmp(ros_ptr(p + SP_NAME), key, 12))
                break;
            uint32_t next = ros_ld32(p + SP_NEXT);
            if (!next)
                return sx_err(SX_DOESNTEXIST);
            p += next;
        }
        reg = p;
        if (hi == 0)
            return (*sp = reg, NULL);
    }
    if ((reg & 3) || reg < 256)
        return sx_err(SX_BADADDRESS);
    *sp = reg;
    return NULL;
}

/* in_area is findsprite_inarea, for the calls that work on the area
 * (append, palettes, rows and columns, alpha).  Otherwise the lookup is
 * findsprite.  With a sprite pointer (&2xx) findsprite does not look at R1
 * at all.  It puts 256 there, "something that passes
 * CheckAlignedAndSensible".  The plotting calls (PutSpriteScaled,
 * PlotMaskScaled, the transformed ones, TileSpriteScaled) and
 * SetPointerShape use findsprite.  OvationPro plots a document's pictures
 * with SpriteOp &238 and R1 = &FF (#154). */
os_error *sx_find(struct ros_cpu *s, struct sx_call *c, int in_area)
{
    c->code = s->r[0];
    uint32_t area = s->r[1];
    if (!in_area && (c->code & ~0xFFu) == 0x200)
        area = 256;
    return find_named(c->code, area, s->r[2], &c->area, &c->sp);
}

/* ---- memory moves ---------------------------------------------------------------------- */

static void mem_move(uint32_t to, uint32_t from, uint32_t n)
{
    if (n && to != from)
        memmove(ros_ptr(to), ros_ptr(from), n);
}

/* Open a zero-filled gap of n bytes at p, moving [p, free) up */
static void gap_open(uint32_t area, uint32_t p, uint32_t n)
{
    uint32_t free = area + ros_ld32(area + SA_FREE);
    mem_move(p + n, p, free - p);
    memset(ros_ptr(p), 0, n);
}

/* Close n bytes at p, moving [p+n, free) down */
static void gap_close(uint32_t area, uint32_t p, uint32_t n)
{
    uint32_t free = area + ros_ld32(area + SA_FREE);
    mem_move(p, p + n, free - (p + n));
}

static void add32(uint32_t a, int32_t v)
{
    ros_st32(a, ros_ld32(a) + (uint32_t)v);
}

/* GetMaskspWidth / FindMaskWidth: a new-format mask's words and last bit */
static void mask_width(uint32_t sp, uint32_t l2, uint32_t width, uint32_t rbit, uint32_t *words,
                       uint32_t *lastbit)
{
    uint32_t px = ((rbit + 1) >> l2) + (width << (5 - l2));
    if (ros_ld32(sp + SP_MODE) & 0x80000000u)
        px <<= 3;
    *words = (px + 31) >> 5;
    *lastbit = ((px & 31) - 1) & 31;
}

static uint32_t sprite_l2bpp(uint32_t sp, os_error **e)
{
    struct sx_mode m;
    *e = NULL;
    if (sx_read_mode(ros_ld32(sp + SP_MODE), &m)) {
        *e = sx_err(SX_BADMODE);
        return 0;
    }
    return m.l2bpp;
}

/* ---- 17 CheckSpriteArea -------------------------------------------------------------------- */

static int within(uint32_t p, uint32_t off, uint32_t lim)
{
    return !(off & 3) && p + off >= p && p + off <= lim;
}

static os_error *check_area(struct ros_cpu *s)
{
    if ((s->r[0] >> 8) == 0)
        return NULL;
    uint32_t a = s->r[1];
    if ((a & 3) || a < 256)
        return sx_err(SX_BADDATA);
    uint32_t e = ros_ld32(a), n = ros_ld32(a + 4), f = ros_ld32(a + 8), u = ros_ld32(a + 12);
    if ((f | u) & 3 || f > u || u > e)
        return sx_err(SX_BADDATA);
    static const uint8_t bpp_of[19] = { 99, 0, 1, 2, 3, 4, 5, 5, 5, 5, 4, 5, 5, 5, 5, 5, 4, 5, 5 };
    uint32_t used = a + u, aend = a + e, p = a + f;
    for (; n; n--) {
        if (p + 44 > used)
            return sx_err(SX_BADDATA);
        uint32_t next = ros_ld32(p), img = ros_ld32(p + SP_IMAGE), trn = ros_ld32(p + SP_TRANS);
        uint32_t lb = ros_ld32(p + SP_LBIT), rb = ros_ld32(p + SP_RBIT), m = ros_ld32(p + SP_MODE);
        uint32_t w = ros_ld32(p + SP_WIDTH), h = ros_ld32(p + SP_HEIGHT);
        if (!within(p, next, used) || !within(p, img, aend) || !within(p, trn, aend))
            return sx_err(SX_BADDATA);
        if ((lb | rb) >= 32 || ((m >> 27) && lb))
            return sx_err(SX_BADDATA);
        uint32_t bytes = (w + 1) * 4 * (h + 1);
        if (img == trn) {
            if (next - img < bytes)
                return sx_err(SX_BADDATA);
        } else {
            if (trn - img < bytes)
                return sx_err(SX_BADDATA);
            uint32_t t = m & (15u << 27), l2;
            if (t == 0) {
                if ((m & 0x80000000u) || next - trn < bytes || trn - img != next - trn)
                    return sx_err(SX_BADDATA);
                goto nextsprite;
            } else if (t == 15u << 27) {
                if ((m & 0xF0000) && ((m ^ 1) & 0xF))
                    return sx_err(SX_BADDATA);
                uint32_t ty = (m >> 20) & 127;
                l2 = bpp_of[ty > 18 ? 6 : ty];
            } else {
                if (!(m & 1) || !((m >> 1) & 0x1FFF) || !((m >> 14) & 0x1FFF))
                    return sx_err(SX_BADDATA);
                l2 = bpp_of[m >> 27 & 15];
            }
            uint32_t px = ((rb + 1) >> l2) + (w << (5 - l2));
            if (m & 0x80000000u)
                px <<= 3;
            uint32_t rowbytes = ((px + 31) & ~31u) >> 3;
            if (next != trn + rowbytes * (h + 1))
                return sx_err(SX_BADDATA);
        }
nextsprite:
        p += next;
    }
    return NULL;
}

/* ---- 37 CreateRemovePalette ------------------------------------------------------------------ */

static os_error *palette_op(struct ros_cpu *s, struct sx_call *c)
{
    uint32_t sp = c->sp, a = c->area;
    uint32_t img = ros_ld32(sp + SP_IMAGE), trn = ros_ld32(sp + SP_TRANS);
    uint32_t lo = img < trn ? img : trn, pal = sp + 44;
    if (s->r[3] == 0xFFFFFFFFu) {
        uint32_t n = (uint32_t)((int32_t)(lo - 44) >> 3);
        s->r[3] = n, s->r[4] = n ? pal : 0, s->r[5] = ros_ld32(sp + SP_MODE);
        return NULL;
    }
    if (s->r[3] == 0) {
        uint32_t sz = lo - 44;
        if (sz)
            gap_close(a, pal, sz);
        add32(a + SA_FREE, -(int32_t)sz), add32(sp + SP_NEXT, -(int32_t)sz);
        add32(sp + SP_IMAGE, -(int32_t)sz), add32(sp + SP_TRANS, -(int32_t)sz);
        return NULL;
    }
    uint32_t m = ros_ld32(sp + SP_MODE);
    if (m >> 27) {                              /* TestForTrueColour */
        uint32_t t = (m >> 27) & 15;
        if (t == 15)
            t = (m >> 20) & 127;
        if (t >= 5)
            return sx_err(SX_BADDEPTH);
    }
    os_error *e;
    uint32_t l2 = sprite_l2bpp(sp, &e);
    if (e)
        return e;
    uint32_t max = l2 >= 3 ? 255 : (1u << (1u << l2)) - 1;
    uint32_t n = max & (s->r[3] & 0x80000000u ? 255 : 63);
    int32_t need = (int32_t)((n + 1) * 8), cur = (int32_t)(lo - 44);
    static const uint32_t bpp1[2] = { 0, 0xFFFFFF00u };
    static const uint32_t bpp2[4] = { 0, 0x0000FF00u, 0x00FFFF00u, 0xFFFFFF00u };
    static const uint32_t bpp4[16] = {
        0x00000000u, 0x0000FF00u, 0x00FF0000u, 0x00FFFF00u, 0xFF000000u, 0xFF00FF00u,
        0xFFFF0000u, 0xFFFFFF00u, 0x00000000u, 0x0000FF00u, 0x00FF0000u, 0x00FFFF00u,
        0xFF000000u, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFF00u,
    };
    uint32_t bpp8[16];
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t t = 0x10 * (i & 3);
        bpp8[i] = t << 8 | t << 16 | t << 24 | (i & 4 ? 0x4000u : 0) | (i & 8 ? 0x40000000u : 0);
    }
    const uint32_t *tab = l2 == 0 ? bpp1 : l2 == 1 ? bpp2 : l2 == 2 ? bpp4 : bpp8;
    uint32_t src_existing = cur != 0;
    uint32_t oldpal[16];
    if (src_existing)
        for (uint32_t i = 0; i < 16; i++)
            oldpal[i] = ros_ld32(pal + 8 * i);
    if (cur)
        need -= cur;
    uint32_t free = ros_ld32(a + SA_FREE), end = ros_ld32(a + SA_END);
    if ((uint32_t)((int32_t)free + need) > end)
        return sx_err(SX_NOTENOUGHROOM);
    if (need > 0)
        gap_open(a, pal + (uint32_t)cur, (uint32_t)need);
    else if (need < 0)
        gap_close(a, pal + (uint32_t)cur + (uint32_t)need, (uint32_t)-need);
    add32(a + SA_FREE, need), add32(sp + SP_NEXT, need);
    add32(sp + SP_IMAGE, need), add32(sp + SP_TRANS, need);
    for (uint32_t i = n + 1; i-- > 0;) {
        uint32_t v = src_existing ? oldpal[i & 15] : tab[(i & 15) % (max < 15 ? max + 1 : 16)];
        if (max == 255) {
            v = (v & ~0x80C08000u) | ((i & 0x80) << 24) | ((i & 0x60) << 17) | ((i & 0x10) << 11);
            v &= ~0x0F0F0F00u;
            v |= v >> 4;
        }
        v |= 0x10;
        ros_st32(pal + i * 8, v);
        ros_st32(pal + i * 8 + 4, v);
    }
    return NULL;
}

/* ---- 57, 58 InsertDeleteRows and InsertDeleteColumns --------------------------------------- */

static uint32_t mask_row_bytes(uint32_t sp, uint32_t l2)
{
    uint32_t irb = (ros_ld32(sp + SP_WIDTH) + 1) * 4;
    if (!(ros_ld32(sp + SP_MODE) >> 27))
        return irb;
    uint32_t w, lb;
    mask_width(sp, l2, ros_ld32(sp + SP_WIDTH), ros_ld32(sp + SP_RBIT), &w, &lb);
    return w * 4;
}

static os_error *rows_op(struct ros_cpu *s, struct sx_call *c)
{
    int32_t at = (int32_t)s->r[3], by = (int32_t)s->r[4];
    if (by == 0)
        return NULL;
    uint32_t sp = c->sp, a = c->area;
    os_error *e;
    uint32_t l2 = sprite_l2bpp(sp, &e);
    if (e)
        return e;
    int32_t h = (int32_t)ros_ld32(sp + SP_HEIGHT) + 1;
    uint32_t irb = (ros_ld32(sp + SP_WIDTH) + 1) * 4, mrb = mask_row_bytes(sp, l2);
    int mask = ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS);
    if (by < 0) {
        int32_t n = -by, top = at + n - 1;
        if (at < 0 || at >= h || top < 0 || top >= h || n >= h)
            return sx_err(SX_INVRC);
        uint32_t memrow = (uint32_t)(h - top - 1);
        if (mask) {
            gap_close(a, sp + ros_ld32(sp + SP_TRANS) + memrow * mrb, (uint32_t)n * mrb);
            add32(a + SA_FREE, -n * (int32_t)mrb), add32(sp + SP_NEXT, -n * (int32_t)mrb);
        }
        gap_close(a, sp + ros_ld32(sp + SP_IMAGE) + memrow * irb, (uint32_t)n * irb);
        add32(a + SA_FREE, -n * (int32_t)irb);
        if (mask)
            add32(sp + SP_TRANS, -n * (int32_t)irb);
        add32(sp + SP_NEXT, -n * (int32_t)irb);
        ros_st32(sp + SP_HEIGHT, (uint32_t)(h - 1 - n));
        return NULL;
    }
    int32_t n = by;
    if (at < 0 || at > h)
        return sx_err(SX_INVRC);
    uint32_t need = ros_ld32(a + SA_FREE) + (uint32_t)n * irb + (mask ? (uint32_t)n * mrb : 0);
    if ((int32_t)need > (int32_t)ros_ld32(a + SA_END))
        return sx_err(SX_NOIROOM);
    uint32_t memrow = (uint32_t)(h - at);
    if (mask) {
        gap_open(a, sp + ros_ld32(sp + SP_TRANS) + memrow * mrb, (uint32_t)n * mrb);
        add32(a + SA_FREE, n * (int32_t)mrb), add32(sp + SP_NEXT, n * (int32_t)mrb);
    }
    gap_open(a, sp + ros_ld32(sp + SP_IMAGE) + memrow * irb, (uint32_t)n * irb);
    add32(a + SA_FREE, n * (int32_t)irb);
    if (mask)
        add32(sp + SP_TRANS, n * (int32_t)irb);
    add32(sp + SP_NEXT, n * (int32_t)irb);
    ros_st32(sp + SP_HEIGHT, (uint32_t)(h - 1 + n));
    return NULL;
}

static uint32_t shl(uint32_t v, uint32_t n)
{
    return n >= 32 ? 0 : v << n;
}

static uint32_t shr(uint32_t v, uint32_t n)
{
    return n >= 32 ? 0 : v >> n;
}

/* del_cols removes bits first..last from each of rows rows of W+1 words
 * (last bit rb).  The rows are packed down in place.  It returns the number
 * of bytes freed */
static uint32_t del_cols(uint32_t a, uint32_t data, uint32_t W, uint32_t rb, uint32_t rows,
                         uint32_t first, uint32_t last)
{
    uint32_t src = data, dst = data;
    for (uint32_t row = 0; row < rows; row++) {
        uint32_t copied = 0;
        for (int32_t t = (int32_t)first - 32; t >= 0; t -= 32) {
            ros_st32(dst, ros_ld32(src)), dst += 4, src += 4;
            copied += 32;
        }
        uint32_t k = first & 31, acc = ros_ld32(src) & (k ? (1u << k) - 1 : 0), nv;
        uint32_t e = last - copied;
        while (e >= 32)
            src += 4, e -= 32;
        uint32_t w = shr(ros_ld32(src), e) >> 1;
        src += 4;
        acc |= shl(w, k);
        if (k > e) {
            ros_st32(dst, acc), dst += 4;
            acc = shr(w, 32 - k);
            nv = k - e - 1;
        } else {
            nv = k + 31 - e;
        }
        for (uint32_t idx = last >> 5; idx < W; idx++) {
            uint32_t v = ros_ld32(src);
            src += 4;
            acc |= shl(v, nv);
            ros_st32(dst, acc), dst += 4;
            acc = shr(v, 32 - nv);
        }
        if (nv > 31 - rb)
            ros_st32(dst, acc), dst += 4;
    }
    uint32_t freed = src - dst;
    gap_close(a, dst, freed);
    return freed;
}

/* insert_columns_in_data: nb zero bits at start in each row, rows widened
 * from W+1 to W+1+extra words; working backwards, in a gap already open */
static void ins_cols(uint32_t data, uint32_t W, uint32_t rb, uint32_t rows, uint32_t start,
                     uint32_t nb, uint32_t extra)
{
    uint32_t ow = W + 1, nw = ow + extra, oldlast = 32 * W + rb;
    uint32_t *tmp = calloc(nw + 1, 4), *row = calloc(ow + 1, 4);
    for (uint32_t r = rows; r-- > 0;) {
        memcpy(row, ros_ptr(data + r * ow * 4), ow * 4);
        memset(tmp, 0, nw * 4);
        for (uint32_t b = 0; b <= oldlast + nb && b < nw * 32; b++) {
            uint32_t bit;
            if (b < start)
                bit = row[b >> 5] >> (b & 31) & 1;
            else if (b < start + nb)
                bit = 0;
            else
                bit = row[(b - nb) >> 5] >> ((b - nb) & 31) & 1;
            tmp[b >> 5] |= bit << (b & 31);
        }
        memcpy(ros_ptr(data + r * nw * 4), tmp, nw * 4);
    }
    free(tmp), free(row);
}

static os_error *cols_op(struct ros_cpu *s, struct sx_call *c)
{
    int32_t col = (int32_t)s->r[3], by = (int32_t)s->r[4];
    if (by == 0)
        return NULL;
    uint32_t sp = c->sp, a = c->area, m = ros_ld32(sp + SP_MODE);
    struct sx_mode md;
    if (sx_read_mode(m, &md))
        return sx_err(SX_BADMODE);
    uint32_t l2 = md.l2bpp, lb = ros_ld32(sp + SP_LBIT), rb = ros_ld32(sp + SP_RBIT);
    uint32_t w = ros_ld32(sp + SP_WIDTH), h = ros_ld32(sp + SP_HEIGHT) + 1;
    int mask = ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS), newfmt = (m >> 27) != 0;
    uint32_t mul = m & 0x80000000u ? 8 : 1, mw, mlast;
    mask_width(sp, l2, w, rb, &mw, &mlast);
    if (col < 0)
        return sx_err(SX_INVRC);
    if (by < 0) {
        uint32_t n = (uint32_t)-by;
        uint32_t first = (uint32_t)col << l2, last = (((uint32_t)col + n) << l2) - 1;
        if (col)
            first += lb, last += lb;
        int32_t d = (int32_t)(last - w * 32);
        if (d > (int32_t)rb || (d == (int32_t)rb && first == 0))
            return sx_err(SX_INVRC);
        if (mask) {
            uint32_t mf = first, ml = last, mwd = w, mrb = rb;
            if (newfmt) {
                mf = (uint32_t)col * mul, ml = ((uint32_t)col + n) * mul - 1;
                if (col)
                    mf += lb, ml += lb;
                mwd = mw - 1, mrb = mlast;
            }
            uint32_t dm = del_cols(a, sp + ros_ld32(sp + SP_TRANS), mwd, mrb, h, mf, ml);
            add32(sp + SP_NEXT, -(int32_t)dm), add32(a + SA_FREE, -(int32_t)dm);
        }
        uint32_t di = del_cols(a, sp + ros_ld32(sp + SP_IMAGE), w, rb, h, first, last);
        if (mask)
            add32(sp + SP_TRANS, -(int32_t)di);
        add32(sp + SP_NEXT, -(int32_t)di), add32(a + SA_FREE, -(int32_t)di);
        if (first == 0)
            ros_st32(sp + SP_LBIT, 0);
        uint32_t lastbit = rb + w * 32 - (last - first + 1);
        ros_st32(sp + SP_RBIT, lastbit & 31);
        ros_st32(sp + SP_WIDTH, lastbit >> 5);
        return NULL;
    }
    uint32_t n = (uint32_t)by, start = ((uint32_t)col << l2) + lb, nb = n << l2;
    if ((int32_t)start > (int32_t)(rb + w * 32 + 1))
        return sx_err(SX_INVRC);
    uint32_t xi = ((rb + nb) >> 5) * h;
    uint32_t xm = !mask ? 0 : !newfmt ? xi : ((mlast + ((nb * mul) >> l2)) >> 5) * h;
    if ((int32_t)(ros_ld32(a + SA_FREE) + 4 * (xi + xm)) > (int32_t)ros_ld32(a + SA_END))
        return sx_err(SX_NOIROOM);
    uint32_t at = sp + (mask ? ros_ld32(sp + SP_TRANS) : ros_ld32(sp + SP_NEXT));
    gap_open(a, at, 4 * xi);
    ins_cols(sp + ros_ld32(sp + SP_IMAGE), w, rb, h, start, nb, xi / h);
    add32(sp + SP_NEXT, 4 * (int32_t)xi), add32(a + SA_FREE, 4 * (int32_t)xi);
    if (mask) {
        add32(sp + SP_TRANS, 4 * (int32_t)xi);
        gap_open(a, sp + ros_ld32(sp + SP_NEXT), 4 * xm);
        if (newfmt)
            ins_cols(sp + ros_ld32(sp + SP_TRANS), mw - 1, mlast, h, (start * mul) >> l2,
                     n * mul, xm / h);
        else
            ins_cols(sp + ros_ld32(sp + SP_TRANS), w, rb, h, start, nb, xm / h);
        add32(sp + SP_NEXT, 4 * (int32_t)xm), add32(a + SA_FREE, 4 * (int32_t)xm);
    }
    ros_st32(sp + SP_WIDTH, w + ((rb + nb) >> 5));
    ros_st32(sp + SP_RBIT, (rb + nb) & 31);
    return NULL;
}

/* ---- 35 AppendSprite ----------------------------------------------------------------------- */

/* Rotate [a, b) and [b, c): [b, c) first */
static void rotate(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t n1 = b - a, n2 = c - b;
    uint8_t *t = malloc(n1 ? n1 : 1);
    memcpy(t, ros_ptr(a), n1);
    memmove(ros_ptr(a), ros_ptr(b), n2);
    memcpy(ros_ptr(a + n2), t, n1);
    free(t);
}

/* Interleave rows: [r0..rn][s0..sn] -> r0 s0 r1 s1 ... */
static void merge_rows(uint32_t p, uint32_t rows, uint32_t rb1, uint32_t rb2)
{
    uint32_t total = rows * (rb1 + rb2);
    uint8_t *t = malloc(total ? total : 1), *q = t;
    const uint8_t *a = ros_ptr(p), *b = a + rows * rb1;
    for (uint32_t r = 0; r < rows; r++) {
        memcpy(q, a + r * rb1, rb1), q += rb1;
        memcpy(q, b + r * rb2, rb2), q += rb2;
    }
    memcpy(ros_ptr(p), t, total);
    free(t);
}

/* chunterrow: the kernel of the in-place bit packer */
static void chunter_row(uint32_t *in, uint32_t *out, uint32_t W, uint32_t lb, uint32_t *r7p,
                        uint32_t rb)
{
    uint32_t r7 = *r7p, rbx = rb + 1;
    uint32_t r0 = shr(ros_ld32(*in), lb);
    *in += 4;
    uint32_t r10 = 32 - r7;
    uint32_t r14 = shr(shl(ros_ld32(*out), r10), r10);
    r14 |= shl(r0, r7);
    uint32_t n = W == 0 ? rbx - lb : 32 - lb;
    r7 += n;
    if (r7 >= 32) {
        r7 -= 32;
        ros_st32(*out, r14), *out += 4;
        r14 = shr(r0, r10);
    }
    uint32_t r9 = 32 - r7;
    if (W != 0) {
        W -= 1;
        if (W != 0) {
            if (r7 == 0) {
                for (uint32_t i = 0; i < W; i++)
                    ros_st32(*out, ros_ld32(*in)), *out += 4, *in += 4;
                r14 = 0;
            } else {
                for (uint32_t i = 0; i < W; i++) {
                    uint32_t v = ros_ld32(*in);
                    *in += 4;
                    r14 |= shl(v, r7);
                    ros_st32(*out, r14), *out += 4;
                    r14 = shr(v, r9);
                }
            }
        }
        uint32_t v = ros_ld32(*in);
        *in += 4;
        r14 |= shl(v, r7);
        r7 += rbx;
        if (r7 >= 32) {
            r7 -= 32;
            ros_st32(*out, r14), *out += 4;
            r14 = shr(v, r9);
        }
    }
    if (r7 != 0)
        ros_st32(*out, r14);
    *r7p = r7;
}

struct sphdr {
    uint32_t width, height, lbit, rbit, image, trans;
};

static void chunter_block(uint32_t *in, uint32_t *out, int horiz, const struct sphdr *a,
                          const struct sphdr *b, uint32_t *width, uint32_t *rbit, uint32_t *height)
{
    uint32_t r7 = 0, rowstart = *out;
    if (horiz) {
        for (uint32_t r = 0; r <= a->height; r++) {
            rowstart = *out, r7 = 0;
            chunter_row(in, out, a->width, a->lbit, &r7, a->rbit);
            chunter_row(in, out, b->width, b->lbit, &r7, b->rbit);
            if (r7)
                *out += 4;
            else
                r7 = 32;
        }
        *height = a->height;
    } else {
        for (uint32_t r = 0; r <= a->height; r++) {
            r7 = 0;
            chunter_row(in, out, a->width, a->lbit, &r7, a->rbit);
            if (r7)
                *out += 4;
        }
        for (uint32_t r = 0; r <= b->height; r++) {
            rowstart = *out, r7 = 0;
            chunter_row(in, out, b->width, b->lbit, &r7, b->rbit);
            if (r7 == 0)
                r7 = 32;
            else
                *out += 4;
        }
        *height = a->height + b->height + 1;
    }
    *width = (*out - rowstart) / 4 - 1;
    *rbit = r7 - 1;
}

static os_error *append(struct ros_cpu *s, struct sx_call *c)
{
    uint32_t a = c->area, s1 = c->sp, s2;
    os_error *e = find_named(c->code, s->r[1], s->r[3], &a, &s2);
    if (e)
        return e;
    int horiz = s->r[4] == 0;
    if (s1 == s2 || ros_ld32(s1 + SP_MODE) != ros_ld32(s2 + SP_MODE))
        return sx_err(SX_APPERR);
    uint32_t mode = ros_ld32(s1 + SP_MODE);
    if (horiz) {
        if (ros_ld32(s1 + SP_HEIGHT) != ros_ld32(s2 + SP_HEIGHT))
            return sx_err(SX_APPERR);
    } else {
        uint32_t w1 = ros_ld32(s1 + SP_WIDTH) * 32 + ros_ld32(s1 + SP_RBIT) - ros_ld32(s1 + SP_LBIT);
        uint32_t w2 = ros_ld32(s2 + SP_WIDTH) * 32 + ros_ld32(s2 + SP_RBIT) - ros_ld32(s2 + SP_LBIT);
        if (w1 != w2)
            return sx_err(SX_APPERR);
    }
    int m1 = ros_ld32(s1 + SP_IMAGE) != ros_ld32(s1 + SP_TRANS);
    int m2 = ros_ld32(s2 + SP_IMAGE) != ros_ld32(s2 + SP_TRANS);
    if (m1 != m2) {
        uint32_t r[10] = { 0x200 + 29, a, m1 ? s2 : s1 };
        sx_swi(XOS_SpriteOp, r, &e);
        if (e)
            return e;
    }
    /* again, from the caller's registers: the mask may have moved them */
    if ((e = find_named(c->code, s->r[1], s->r[2], &a, &s1)) != NULL ||
        (e = find_named(c->code, s->r[1], s->r[3], &a, &s2)) != NULL)
        return e;
    int masked = ros_ld32(s1 + SP_IMAGE) != ros_ld32(s1 + SP_TRANS);
    uint32_t size1 = ros_ld32(s1 + SP_NEXT), size2 = ros_ld32(s2 + SP_NEXT);
    uint32_t end = a + ros_ld32(a + SA_FREE);
    rotate(s2, s2 + size2, end);
    uint32_t s2n = end - size2;
    if (s1 >= s2)
        s1 -= size2;
    rotate(s1, s1 + size1, s2n);
    s1 = s2n - size1;
    s2 = s2n;
    struct sphdr h1 = { ros_ld32(s1 + SP_WIDTH), ros_ld32(s1 + SP_HEIGHT), ros_ld32(s1 + SP_LBIT),
                        ros_ld32(s1 + SP_RBIT), ros_ld32(s1 + SP_IMAGE), ros_ld32(s1 + SP_TRANS) };
    struct sphdr h2 = { ros_ld32(s2 + SP_WIDTH), ros_ld32(s2 + SP_HEIGHT), ros_ld32(s2 + SP_LBIT),
                        ros_ld32(s2 + SP_RBIT), ros_ld32(s2 + SP_IMAGE), ros_ld32(s2 + SP_TRANS) };
    /* sprite 2's header and palette go */
    mem_move(s2, s2 + h2.image, size2 - h2.image);
    uint32_t img1 = s1 + h1.image, img2 = s2;
    uint32_t irb1 = (h1.width + 1) * 4, irb2 = (h2.width + 1) * 4;
    uint32_t isz1 = irb1 * (h1.height + 1), isz2 = irb2 * (h2.height + 1);
    uint32_t mask1 = 0;
    if (masked) {
        uint32_t msz1 = size1 - h1.trans;
        rotate(s1 + h1.trans, s1 + h1.trans + msz1, img2 + isz2);  /* [mask1][img2] swapped */
        mask1 = img1 + isz1 + isz2;
        h1.trans = mask1 - s1;
        if (horiz)
            merge_rows(img1, h1.height + 1, irb1, irb2);
        uint32_t l2 = 0;
        if (mode >> 27) {
            struct sx_mode md;
            if (!sx_read_mode(mode, &md))
                l2 = md.l2bpp;
        }
        if (horiz) {
            if (!(mode >> 27)) {
                merge_rows(mask1, h1.height + 1, irb1, irb2);
            } else {
                uint32_t mw1, mw2, lb1, lb2;
                mask_width(s1, l2, h1.width, h1.rbit, &mw1, &lb1);
                mask_width(s1, l2, h2.width, h2.rbit, &mw2, &lb2);
                merge_rows(mask1, h1.height + 1, mw1 * 4, mw2 * 4);
            }
        }
    } else if (horiz) {
        merge_rows(img1, h1.height + 1, irb1, irb2);
    }
    uint32_t in = img1, out = img1, nw, nrb, nh;
    chunter_block(&in, &out, horiz, &h1, &h2, &nw, &nrb, &nh);
    ros_st32(s1 + SP_WIDTH, nw), ros_st32(s1 + SP_HEIGHT, nh);
    ros_st32(s1 + SP_LBIT, 0), ros_st32(s1 + SP_RBIT, nrb);
    if (masked) {
        ros_st32(s1 + SP_TRANS, out - s1);
        uint32_t min = mask1, mout = out;
        if (!(mode >> 27)) {
            uint32_t w, rb, hh;
            chunter_block(&min, &mout, horiz, &h1, &h2, &w, &rb, &hh);
            ros_st32(s1 + SP_WIDTH, w), ros_st32(s1 + SP_HEIGHT, hh);
            ros_st32(s1 + SP_LBIT, 0), ros_st32(s1 + SP_RBIT, rb);
        } else {
            struct sx_mode md;
            uint32_t l2 = sx_read_mode(mode, &md) ? 0 : md.l2bpp, mw1, mw2, lb1, lb2, w, rb, hh;
            mask_width(s1, l2, h1.width, h1.rbit, &mw1, &lb1);
            mask_width(s1, l2, h2.width, h2.rbit, &mw2, &lb2);
            struct sphdr k1 = { mw1 - 1, h1.height, 0, lb1, 0, 0 };
            struct sphdr k2 = { mw2 - 1, h2.height, 0, lb2, 0, 0 };
            chunter_block(&min, &mout, horiz, &k1, &k2, &w, &rb, &hh);
        }
        out = mout;
    }
    ros_st32(s1 + SP_NEXT, out - s1);
    ros_st32(a + SA_FREE, out - a);
    add32(a + SA_NUMBER, -1);
    return NULL;
}

/* ---- 38 CreateRemoveAlpha ----------------------------------------------------------------------
 *
 * Moves a sprite's transparency between its three forms, or removes it.
 * The forms are a 1bpp mask, an 8bpp (alpha) mask, and an alpha channel in
 * the pixels (16bpp 1555 or 4444, 32bpp 8888).  This follows SprOp's
 * Go_CreateRemoveAlpha step for step.  The in-place conversions run in the
 * same order over the same bytes, so the padding bits they leave are the
 * original's. */

enum {
    CRA_1MASK = 1, CRA_8MASK = 2, CRA_MASK = 3, CRA_1ALPHA = 4, CRA_4ALPHA = 8, CRA_8ALPHA = 16,
    CRA_ALPHA = 28, CRA_BINARY = 32, CRA_SOLID = 64, CRA_4BPP = 128, CRA_ABOVECUT = 256,
    CRA_ABOVE240 = 512, CRA_PROPS = 0x3E0,
};

/* ExamineAlpha's result: the transparency's kind, its first row, words per
 * row and last bit index; what clears it from the pixels (DestroySrcAlpha) */
struct cra {
    uint32_t kind, row, words, last, clear;
};

/* The Get*Pixel readers: the value 0-255 at bit offset *bit of row, and on */
static uint32_t cra_read(const struct cra *x, uint32_t row, uint32_t *bit)
{
    uint32_t b = *bit, v;
    switch (x->kind) {
    case CRA_1MASK: v = (ros_ld8(row + (b >> 3)) >> (b & 7)) & 1 ? 255 : 0, b += 1; break;
    case CRA_8MASK: v = ros_ld8(row + (b >> 3)), b += 8; break;
    case CRA_1ALPHA: b += 16, v = ros_ld8(row + (b >> 3) - 1) & 128 ? 255 : 0; break;
    case CRA_4ALPHA: b += 16, v = ros_ld8(row + (b >> 3) - 1) & 0xF0, v |= v >> 4; break;
    default: b += 32, v = ros_ld8(row + (b >> 3) - 1); break;
    }
    *bit = b;
    return v;
}

/* GetNonAlphaEquivalent, with its demotion that always gives 90 dpi */
static uint32_t cra_non_alpha(uint32_t m)
{
    m &= ~0x80000000u;
    if ((m >> 27) != 15 || (m & 0x3000) || (m & 0xE) || (m & 0xF0000))
        return m;
    m &= ~0x8000u;
    if ((m & 0xFF00) || (m & (112u << 20)))
        return m;
    uint32_t dpi = 180u >> (m & 3);
    return ((m >> 20) & 127) << 27 | dpi << 1 | dpi << 14 | 1;
}

static int cra_eig(uint32_t dpi)
{
    return dpi == 180 ? 0 : dpi == 90 ? 1 : dpi == 45 ? 2 : dpi == 23 || dpi == 22 ? 3 : -1;
}

/* GetAlphaEquivalent: the mode word with an alpha channel, and its bits */
static os_error *cra_alpha(uint32_t m, uint32_t *out, uint32_t *bits)
{
    m &= ~0x80000000u;
    if ((m >> 27) != 15) {
        int xe = cra_eig((m >> 1) & 0x1FFF), ye = cra_eig((m >> 14) & 0x1FFF);
        if (xe < 0 || ye < 0)
            return sx_err(SX_BADMODE);
        m = 1 | (uint32_t)xe << 4 | (uint32_t)ye << 6 | 15u << 27 | (m >> 27) << 20;
    }
    uint32_t t = (m >> 20) & 127;
    *bits = t == 5 ? 1 : t == 6 ? 8 : t == 16 ? 4 : 0;
    if ((m & 0x3000) || !*bits)
        return sx_err(SX_BADMODE);
    *out = m | 0x8000;
    return NULL;
}

/* FindMaskWidth for the mode word m: words per row, and last bit index */
static void cra_mask_width(uint32_t sp, uint32_t l2, uint32_t m, uint32_t *words, uint32_t *last)
{
    uint32_t px = ((ros_ld32(sp + SP_RBIT) + 1) >> l2) + (ros_ld32(sp + SP_WIDTH) << (5 - l2));
    if (m & 0x80000000u)
        px <<= 3;
    *words = (px + 31) >> 5;
    *last = (((px & 31) - 1) & 31) + (*words - 1) * 32;
}

static os_error *cra_examine(uint32_t sp, uint32_t r3, struct cra *x, uint32_t *l2, uint32_t *f)
{
    uint32_t m = ros_ld32(sp + SP_MODE), trn = ros_ld32(sp + SP_TRANS);
    uint32_t img = ros_ld32(sp + SP_IMAGE);
    struct sx_mode md;
    *f = 0;
    if (sx_read_mode(m, &md))
        return sx_err(SX_BADMODE);
    *l2 = md.l2bpp;
    if (!(md.flags & 0x8000)) {
        if (trn == img) {               /* no mask: lose any phantom 8bpp one */
            ros_st32(sp + SP_MODE, m & ~0x80000000u);
            return NULL;
        }
        x->kind = m & 0x80000000u ? CRA_8MASK : CRA_1MASK;
        x->row = sp + trn, x->clear = 0;
        cra_mask_width(sp, md.l2bpp, m, &x->words, &x->last);
    } else {
        if (trn != img || (m & 0x80000000u))
            return sx_err(SX_BADMODE);
        if (md.ncolour == 0xFFFFFFFFu)
            x->kind = CRA_8ALPHA, x->clear = 0xFF000000u;
        else if (md.ncolour == 65535)
            x->kind = CRA_1ALPHA, x->clear = 0x80008000u;
        else if (md.ncolour == 4095)
            x->kind = CRA_4ALPHA, x->clear = 0xF000F000u;
        else
            return sx_err(SX_BADMODE);
        x->row = sp + img, x->words = ros_ld32(sp + SP_WIDTH) + 1;
        x->last = ros_ld32(sp + SP_RBIT) + ros_ld32(sp + SP_WIDTH) * 32;
    }
    uint32_t cut = r3 & 255 ? r3 & 255 : 255;
    *f = x->kind | CRA_PROPS;
    uint32_t row = x->row;
    for (uint32_t y = 0; y <= ros_ld32(sp + SP_HEIGHT); y++, row += x->words * 4) {
        uint32_t bit = 0;
        do {
            uint32_t a = cra_read(x, row, &bit);
            if (a == 255)
                continue;
            if (a < cut)
                *f &= ~(uint32_t)CRA_ABOVECUT;
            if (a < 240)
                *f &= ~(uint32_t)CRA_ABOVE240;
            if (a > 0)
                *f &= ~(uint32_t)CRA_BINARY;
            *f &= ~(uint32_t)CRA_SOLID;
            if ((a ^ (a >> 4)) & 15)
                *f &= ~(uint32_t)CRA_4BPP;
            if (!(*f & CRA_PROPS))
                return NULL;
        } while (bit <= x->last);
    }
    return NULL;
}

/* TransferTo1bppMask / TransferTo8bppMask, into the mask at spTrans with
 * dwords words a row; *dend and *send: where the two ended */
static void cra_transfer(uint32_t sp, const struct cra *x, int to8, uint32_t cut, uint32_t dwords,
                         uint32_t *dend, uint32_t *send)
{
    uint32_t d = sp + ros_ld32(sp + SP_TRANS), s = x->row;
    for (uint32_t y = 0; y <= ros_ld32(sp + SP_HEIGHT); y++) {
        uint32_t bit = 0, o = 0;
        do {
            uint32_t a = cra_read(x, s, &bit);
            if (to8) {
                ros_st8(d + o, a);
            } else {
                uint32_t b = ros_ld8(d + (o >> 3)), k = 1u << (o & 7);
                ros_st8(d + (o >> 3), a >= cut ? b | k : b & ~k);
            }
            o++;
        } while (bit <= x->last);
        s += x->words * 4, d += dwords * 4;
    }
    *dend = d, *send = s;
}

static void cra_destroy(uint32_t sp, uint32_t clear)
{
    if (!clear)
        return;
    uint32_t p = sp + ros_ld32(sp + SP_IMAGE);
    uint32_t n = (ros_ld32(sp + SP_WIDTH) + 1) * (ros_ld32(sp + SP_HEIGHT) + 1);
    for (; n; n--, p += 4)
        ros_st32(p, ros_ld32(p) & ~clear);
}

/* ExpandMaskSizeToR0: the mask grown to want bytes (a new one at the
 * sprite's end), the growth filled with 1s; *over, the shortfall */
static os_error *cra_expand(uint32_t a, uint32_t sp, uint32_t want, uint32_t *over)
{
    uint32_t ms = ros_ld32(sp + SP_TRANS), me = ros_ld32(sp + SP_IMAGE);
    if (ms == me)
        ms = me = ros_ld32(sp + SP_NEXT);
    else if ((int32_t)ms > (int32_t)me)
        me = ros_ld32(sp + SP_NEXT);
    uint32_t g = want - (me - ms), fr = ros_ld32(a + SA_FREE);
    int32_t o = (int32_t)(fr + g - ros_ld32(a + SA_END));
    if (o > 0) {
        *over = (uint32_t)o;
        return sx_err(SX_NOTENOUGHROOM);
    }
    ros_st32(a + SA_FREE, fr + g);
    mem_move(sp + me + g, sp + me, a + fr - (sp + me));
    add32(sp + SP_NEXT, (int32_t)g);
    ros_st32(sp + SP_TRANS, ms);
    if ((int32_t)ros_ld32(sp + SP_IMAGE) > (int32_t)ms)
        add32(sp + SP_IMAGE, (int32_t)g);
    memset(ros_ptr(sp + me), 0xFF, g & ~3u);
    return NULL;
}

/* ShrinkAreaR11ToR0: everything from src on moved down to dst */
static void cra_shrink(uint32_t a, uint32_t sp, uint32_t dst, uint32_t src)
{
    int32_t d = (int32_t)(src - dst);
    if (d <= 0)
        return;
    mem_move(dst, src, a + ros_ld32(a + SA_FREE) - src);
    add32(a + SA_FREE, -d), add32(sp + SP_NEXT, -d);
    int32_t o = (int32_t)(src - sp);
    if ((int32_t)ros_ld32(sp + SP_TRANS) >= o)
        add32(sp + SP_TRANS, -d);
    if ((int32_t)ros_ld32(sp + SP_IMAGE) >= o)
        add32(sp + SP_IMAGE, -d);
}

static os_error *cra_remove_mask(uint32_t a, uint32_t sp)
{
    uint32_t r[10] = { 30 + 0x200, a, sp };
    os_error *e;
    sx_swi(XOS_SpriteOp, r, &e);
    return e;
}

static void cra_set_mode(uint32_t sp, uint32_t m)
{
    ros_st32(sp + SP_MODE, m);
}

static os_error *alpha_op(struct ros_cpu *s, struct sx_call *c)
{
    uint32_t a = c->area, sp = c->sp, r3 = s->r[3], m = ros_ld32(sp + SP_MODE);
    s->r[3] = 0;
    if (!(m & (15u << 27))) {
        /* its %0 is R1, the area, not the Title */
        char t[64];
        size_t n = 0;
        for (uint8_t ch; n < sizeof t - 1 && (ch = (uint8_t)ros_ld8(a + n)) >= 32; n++)
            t[n] = (char)ch;
        t[n] = 0;
        return ros_error(SX_BADMODE, "%s: Invalid sprite mode", t);
    }
    struct cra x;
    uint32_t l2, f, words, last, dend, send, cut, bits;
    os_error *e = cra_examine(sp, r3, &x, &l2, &f);
    if (e)
        return e;
    uint32_t target = r3 >> 30, h = ros_ld32(sp + SP_HEIGHT);
    r3 &= 0x3FFFFFFFu;
    if (r3 >= 0x200 || target == 1)
        return sx_err(SX_BADFLAGS);

    if (target == 0) {                                  /* to a 1bpp mask */
        if (!f || (f & CRA_1MASK))
            goto tidy;
        if ((r3 & 0x100) && (f & (CRA_SOLID | CRA_ABOVECUT)))
            goto remove;
        cut = r3 & 255;
        if (!cut) {
            cut = 255;
            if (!(f & CRA_BINARY)) {
                if (f & CRA_8MASK)
                    goto wide;
                return NULL;
            }
        }
        if (!(f & CRA_ALPHA)) {                         /* 8bpp mask, in place */
            cra_transfer(sp, &x, 0, cut, (x.last + 1 + 248) >> 8, &dend, &send);
            cra_shrink(a, sp, dend, send);
            goto tidy;
        }
        cra_mask_width(sp, l2, cra_non_alpha(ros_ld32(sp + SP_MODE)), &words, &last);
        if ((e = cra_expand(a, sp, words * (h + 1) * 4, &s->r[3])))
            return e;
        cra_transfer(sp, &x, 0, cut, words, &dend, &send);
        cra_destroy(sp, x.clear);
        goto tidy;
    }

    if (target == 2) {                                  /* to an 8bpp mask */
        if (f & CRA_8MASK)
            goto wide;
        cra_mask_width(sp, l2, cra_non_alpha(ros_ld32(sp + SP_MODE)) | 0x80000000u, &words, &last);
        if ((e = cra_expand(a, sp, words * (h + 1) * 4, &s->r[3])))
            return e;
        if (!f)
            goto wide;
        if (f & CRA_1MASK) {                            /* 1bpp to 8bpp, backwards */
            uint32_t d = sp + ros_ld32(sp + SP_TRANS) + words * 4 * h;
            uint32_t src = x.row + x.words * 4 * h;
            for (int32_t y = (int32_t)h; y >= 0; y--, d -= words * 4, src -= x.words * 4) {
                uint32_t b = x.last + 1;
                do {
                    b--;
                    ros_st8(d + b, (ros_ld8(src + (b >> 3)) >> (b & 7)) & 1 ? 255 : 0);
                } while ((int32_t)b > 0);
            }
            goto wide;
        }
        cra_transfer(sp, &x, 1, 0, words, &dend, &send);
        cra_destroy(sp, x.clear);
        goto wide;
    }

    /* to an alpha channel */
    if ((r3 & 0x100) && ((f & CRA_SOLID) || !f))
        goto remove;
    if (f & CRA_ALPHA)
        return NULL;
    uint32_t am;
    if ((e = cra_alpha(ros_ld32(sp + SP_MODE), &am, &bits)))
        return e;
    if (!f) {                                           /* opaque from nothing */
        cra_set_mode(sp, am);
        uint32_t k = bits == 1 ? 0x8000u : bits == 4 ? 0xF000u : 0xFF000000u;
        k |= k << 16;
        uint32_t p = sp + ros_ld32(sp + SP_IMAGE);
        for (uint32_t n = (ros_ld32(sp + SP_WIDTH) + 1) * (h + 1); n; n--, p += 4)
            ros_st32(p, ros_ld32(p) | k);
        return NULL;
    }
    if (bits == 1) {
        if ((r3 & 0x100) && (f & CRA_ABOVECUT))
            goto remove;
        cut = r3 & 255;
        if (!cut) {
            cut = 255;
            if (!(f & CRA_BINARY))
                return NULL;
        }
    } else if (bits == 4) {
        if (!(r3 & 255) && !(f & CRA_4BPP))
            return NULL;
        if ((r3 & 0x100) && (f & CRA_ABOVE240))
            goto remove;
        cut = 0;
    } else {
        cut = 0;
    }
    cra_set_mode(sp, am);
    uint32_t step = bits == 8 ? 4 : 2, p = sp + ros_ld32(sp + SP_IMAGE) + (bits == 8 ? 3 : 1);
    uint32_t src = x.row;
    for (uint32_t y = 0; y <= h; y++, src += x.words * 4) {
        uint32_t bit = 0;
        do {
            uint32_t v = cra_read(&x, src, &bit), b = ros_ld8(p);
            if (bits == 1)
                b = v >= cut ? b | 128 : b & ~128u;
            else if (bits == 4)
                b = (b & 0x0F) | (v & 0xF0);
            else
                b = v;
            ros_st8(p, b);
            p += step;
        } while (bit <= x.last);
        if (bits != 8 && (p & 2))
            p += 2;
    }
    /* the mask goes; the kernel sizes it from the mode word, so an 8bpp
     * one needs its flag back for the call */
    uint32_t keep = ros_ld32(sp + SP_MODE);
    if (f & CRA_8MASK)
        cra_set_mode(sp, keep | 0x80000000u);
    e = cra_remove_mask(a, sp);
    cra_set_mode(sp, keep);
    return e;

remove:
    if ((e = cra_remove_mask(a, sp)))
        return e;
tidy:
    cra_set_mode(sp, cra_non_alpha(ros_ld32(sp + SP_MODE)));
    return NULL;
wide:
    cra_set_mode(sp, cra_non_alpha(ros_ld32(sp + SP_MODE)) | 0x80000000u);
    return NULL;
}

/* ---- 36 SetPointerShape ---------------------------------------------------------------------
 *
 * From SprOp's Go_SetPointerShape.  The sprite is plotted, scaled to the
 * screen's pixel shape, and aligned right and top into a 32 x 32 sprite of
 * mode 1 (2 bpp) in a scratch area.  That sprite is given to OS_Word 21 as
 * a pointer shape.  Its palette's first three colours after 0 become the
 * pointer's colours (VDU 19,c,25).  OS_Byte 106 selects the shape.  Bits
 * 4-6 of R3 skip each step. */

enum { SPP_NOIMAGE = 0x10, SPP_NOPALETTE = 0x20, SPP_NOSETNUM = 0x40 };

/* TestForTrueColour: pointers come from 8 bpp or less */
static os_error *spp_depth(uint32_t sp)
{
    uint32_t m = ros_ld32(sp + SP_MODE);
    if (m < 256) {
        uint32_t r[10] = { m, 9 };
        os_error *e;
        if (!sx_swi(XOS_ReadModeVariable, r, &e) && r[2] < 4)
            return NULL;
    } else {
        uint32_t t = (m >> 27) & 15;
        if (t == 15)
            t = (m >> 20) & 127;
        if (t < 5)
            return NULL;
    }
    return sx_err(SX_BADDEPTH);
}

/* go_getactivepointfrommask: the first transparent pixel, in raster order
 * from the top left, becomes the active point (the "PRM 3 vol 3 page 33"
 * fix); the caller's R4 and R5 come back changed */
static void spp_active(uint32_t sp, uint32_t *ax, uint32_t *ay)
{
    if (ros_ld32(sp + SP_LBIT) != 0 || ros_ld32(sp + SP_TRANS) == ros_ld32(sp + SP_IMAGE))
        return;
    uint32_t m = ros_ld32(sp + SP_MODE), r[10] = { m, 9 }, rbit, words1;
    os_error *e;
    sx_swi(XOS_ReadModeVariable, r, &e);
    uint32_t l2 = r[2];
    if (m < 256) {
        rbit = ros_ld32(sp + SP_RBIT), words1 = ros_ld32(sp + SP_WIDTH);
    } else {                            /* a 1 bpp mask */
        uint32_t w = (((ros_ld32(sp + SP_WIDTH) << 5) + ros_ld32(sp + SP_RBIT) + 1) >> l2) - 1;
        words1 = w >> 5, rbit = w - (words1 << 5), l2 = 0;
    }
    uint32_t fill = rbit + 1 >= 32 ? 0 : ~0u << (rbit + 1);
    uint32_t p = sp + ros_ld32(sp + SP_TRANS), h = ros_ld32(sp + SP_HEIGHT);
    uint32_t bpp = 1u << l2, pm = bpp >= 32 ? ~0u : ~(~0u << bpp);
    for (uint32_t y = 0; y <= h; y++) {
        uint32_t x = 0;
        for (uint32_t k = words1 + 1; k--; p += 4) {
            uint32_t w = ros_ld32(p) | (k == 0 ? fill : 0);
            if (w == ~0u) {
                x += 32 >> l2;
                continue;
            }
            for (; (w & pm) == pm; w >>= bpp)
                x++;
            *ax = x, *ay = y;
            return;
        }
    }
}

/* mulR4: v * mul / div, as its unsigned DivRem */
static uint32_t spp_scale(uint32_t v, uint32_t mul, uint32_t div)
{
    return div ? v * mul / div : 0;
}

static os_error *spp_image(struct ros_cpu *s, struct sx_call *c)
{
    uint32_t sp = c->sp;
    os_error *e = spp_depth(sp);
    if (e)
        return e;
    spp_active(sp, &s->r[4], &s->r[5]);
    struct sx_mode sm;
    if (sx_read_mode(ros_ld32(sp + SP_MODE), &sm))
        return sx_err(SX_BADMODE);
    struct sx_dest d;
    sx_read_dest(&d);

    /* the scratch area (on the original's stack): 512 bytes, then its
     * sprite's name, the factors, the hi-res mono table, OS_Word 21's block */
    uint32_t a = ros_addr(ros_rma_alloc(512 + 48));
    uint32_t name = a + 512, fac = a + 520, ttr = a + 536, blk = a + 540;
    ros_st32(a + SA_END, 512), ros_st32(a + SA_NUMBER, 0);
    ros_st32(a + SA_FIRST, 16), ros_st32(a + SA_FREE, 16);
    memcpy(ros_ptr(name), "pointer", 8);
    static const uint8_t hiresmono[4] = { 0, 1, 3, 3 };     /* colour 2 as 3 */
    memcpy(ros_ptr(ttr), hiresmono, 4);
    uint32_t r[10] = { 15 + 0x100, a, name, 0, 32, 32, 1 };
    sx_swi(XOS_SpriteOp, r, &e);
    uint32_t old[10] = { 60 + 0x100, a, name, 0 };
    if (!e)
        sx_swi(XOS_SpriteOp, old, &e);
    if (e) {
        ros_rma_free(ros_ptr(a));
        return e;
    }
    uint32_t f[4];
    if (s->r[6]) {
        for (int i = 0; i < 4; i++)
            f[i] = ros_ld32(s->r[6] + 4u * (uint32_t)i);
    } else {
        f[0] = 1u << sm.xeig, f[1] = 1u << sm.yeig;
        f[2] = (uint32_t)((int32_t)((1u << d.xeig) << d.l2bpp) >> d.l2bpc);
        if (d.flags & 0x10)             /* HiResMono: half width */
            f[2] <<= 1;
        f[3] = 1u << d.yeig;
    }
    for (int i = 0; i < 4; i++)
        ros_st32(fac + 4u * (uint32_t)i, f[i]);
    uint32_t w = ((1 - ros_ld32(sp + SP_LBIT)) + ros_ld32(sp + SP_RBIT) +
                  (ros_ld32(sp + SP_WIDTH) << 5)) >> sm.l2bpc;
    int32_t xoff = 32 - (int32_t)spp_scale(w, f[0], f[2]);
    if (xoff < 0)
        xoff = 0;
    uint32_t h = spp_scale(ros_ld32(sp + SP_HEIGHT) + 1, f[1], f[3]);
    uint32_t t = s->r[7] ? s->r[7] : (d.flags & 0x10) ? ttr : 0;
    uint32_t p[10] = { 52 + 0x200, s->r[1], sp, (uint32_t)xoff * 4, 128 - h * 4, 0, fac, t };
    os_error *lost;
    sx_swi(XOS_SpriteOp, p, &lost);                     /* its error is lost, as there */
    sx_swi(XOS_SpriteOp, old, &lost);                   /* output back */

    uint32_t out = a + ros_ld32(a + SA_FIRST);
    uint32_t img = out + ros_ld32(out + SP_IMAGE);
    uint8_t b[10] = { 0, (uint8_t)(s->r[3] & 15), 8, (uint8_t)(h > 32 ? 32 : h),
                      (uint8_t)(spp_scale(s->r[4], f[0], f[2]) + (uint32_t)xoff),
                      (uint8_t)spp_scale(s->r[5], f[1], f[3]) };
    memcpy(b + 6, &img, 4);
    memcpy(ros_ptr(blk), b, 10);
    uint32_t w21[10] = { 21, blk };
    sx_swi(XOS_Word, w21, &e);
    ros_rma_free(ros_ptr(a));
    return e;
}

/* go_setpointerpalette: pointer colours 3, 2, 1 from the sprite's palette,
 * through R7's table if there is one (searched over the first bpp
 * entries only, as the original does) */
static os_error *spp_palette(struct ros_cpu *s, struct sx_call *c)
{
    uint32_t sp = c->sp;
    struct sx_mode sm;
    if (sx_read_mode(ros_ld32(sp + SP_MODE), &sm))
        return sx_err(SX_BADMODE);
    uint32_t n = 1u << sm.l2bpp;
    for (uint32_t col = 3; col >= 1; col--) {
        uint32_t idx = col;
        if (s->r[7]) {
            for (idx = 0; idx < n && ros_ld8(s->r[7] + idx) != col; idx++)
                ;
            if (idx == n)
                continue;
        }
        uint32_t off = 45 + idx * 8;
        if ((int32_t)ros_ld32(sp + SP_IMAGE) <= (int32_t)off)
            continue;
        uint8_t q[6] = { 19, (uint8_t)col, 25, (uint8_t)ros_ld8(sp + off),
                         (uint8_t)ros_ld8(sp + off + 1), (uint8_t)ros_ld8(sp + off + 2) };
        for (int i = 0; i < 6; i++) {
            uint32_t r[10] = { q[i] };
            os_error *e;
            sx_swi(XOS_WriteC, r, &e);
        }
    }
    return NULL;
}

/* A test aid, off unless rosgd.ptrlog is on the command line (or
 * ROSGD_PTRLOG hosted).  Each SetPointerShape is counted and described in
 * ROSGD$Pointer as "<count> <sprite> &<R3> <x> <y>".  A task can then see
 * what the pointer was set to, which RISC OS gives no way to read */
static void ptrlog(struct ros_cpu *s, struct sx_call *c)
{
    static int on = -1;
    static uint32_t count;
    static char *buf;
    if (on < 0)
        on = getenv("ROSGD_PTRLOG") || ros_cmdline_has("rosgd.ptrlog");
    if (!on)
        return;
    if (!buf) {
        void *mem;
        if (xos_module_claim(96, &mem))
            return;
        buf = mem;
    }
    char name[13];
    for (int i = 0; i < 12; i++)
        name[i] = (char)ros_ld8(c->sp + 4 + (uint32_t)i);
    name[12] = 0;
    snprintf(buf + 32, 64, "%u %s &%X %d %d", ++count, name, s->r[3], (int)s->r[4], (int)s->r[5]);
    strcpy(buf, "ROSGD$Pointer");
    uint32_t r[10] = { ros_addr(buf), ros_addr(buf + 32), (uint32_t)strlen(buf + 32), 0, 0 };
    os_error *e;
    sx_swi(XOS_SetVarVal, r, &e);
}

static os_error *pointer_op(struct ros_cpu *s, struct sx_call *c)
{
    uint32_t r3 = s->r[3];
    os_error *e;
    ptrlog(s, c);
    if (!(r3 & SPP_NOIMAGE) && (e = spp_image(s, c)))
        return e;
    if (!(r3 & SPP_NOPALETTE) && (e = spp_palette(s, c)))
        return e;
    if (!(r3 & SPP_NOSETNUM)) {
        uint32_t r[10] = { 106, r3 & 15, 0 };
        sx_swi(XOS_Byte, r, &e);
        return e;
    }
    return NULL;
}

/* ---- the claim ------------------------------------------------------------------------------ */

static int sprite_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t reason = s->r[0] & 0xFF;
    if (reason != 17 && reason < 35)
        return ROS_VECTOR_PASS;
    struct sx_call c;
    os_error *e = NULL;
    uint32_t code = s->r[0];
    switch (reason) {
    case 17: e = check_area(s); break;
    case 35:
        if (!(e = sx_find(s, &c, 1)))
            e = append(s, &c);
        break;
    case 36:
        if (!(e = sx_find(s, &c, 0)))
            e = pointer_op(s, &c);
        break;
    case 37:
        if (!(e = sx_find(s, &c, 1)))
            e = palette_op(s, &c);
        break;
    case 50:
    case 52: e = sx_scaled(s, reason); break;
    case 51: e = sx_paint_char(s); break;
    case 53: e = sx_err(SX_NOGRSCL); break;
    case 57:
        if (!(e = sx_find(s, &c, 1)))
            e = rows_op(s, &c);
        break;
    case 58:
        if (!(e = sx_find(s, &c, 1)))
            e = cols_op(s, &c);
        break;
    case 38:
        if (!(e = sx_find(s, &c, 1)))
            e = alpha_op(s, &c);
        break;
    case 55:
    case 56: e = sx_transformed(s, reason); break;
    case 65: e = sx_tile(s); break;
    default:
        return ROS_VECTOR_PASS;
    }
    if (e) {
        ros_swi_fail(s, e);
    } else {
        s->v = 0;
        s->r[0] = code;
    }
    return ROS_VECTOR_CLAIM;
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    os_error *e = sx_jpeg_init();
    if (e)
        return e;
    return ros_vector_claim_native(SPRITEV, sprite_v, 0);
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    ros_vector_release_native(SPRITEV, sprite_v, 0);
    sx_jpeg_final();
    return NULL;
}

struct ros_module spriteextend_module = {
    .title = "SpriteExtend",
    .help = "SpriteExtend\t1.99 (2 Oct 2026) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = sx_jpeg_bad_swi,
    .swi_chunk = 0x49980,
    .swi_thunks = ros_swi_thunks_SpriteExtend,
    .swi_names = ros_swi_names_SpriteExtend,
    .swi_prefix = "JPEG",
};

__attribute__((constructor)) static void count(void)
{
    spriteextend_module.swi_count = ros_swi_count_SpriteExtend;
}
