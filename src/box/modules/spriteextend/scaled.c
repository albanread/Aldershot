/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2010 Castle Technology Ltd
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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/Video/Render/SprExtend: Sources/PutScaled, c.PutScaled, c.asmcore,
 * c.sprtrans, h.putscaled).
 */

/* scaled.c -- SpriteExtend's scaled plotting: 52 PutSpriteScaled, 50
 * PlotMaskScaled, 51 PaintCharScaled, 65 TileSpriteScaled.
 *
 * SpriteExtend compiles a plotting loop per call (c/PutScaled, c/asmcore).
 * What that loop does is written here as a function of each destination
 * pixel, from the original's sources.
 *
 *   - The DDA: destination column D (from the sprite's unclipped left edge)
 *     takes source column floor(((D+1)*div - 1)/mag), which is the last of
 *     a shrunk group.  Rows are the same, counted from the bottom.
 *     Clipping does not move the samples.  The one exception is the
 *     dithering and blending loop when it enlarges with a left clip: it
 *     gives the first visible column the first sample.
 *   - The colour comes from a translation table of the caller's (bytes,
 *     words, the 32K kind, or a colour-mapping routine) or from the
 *     sprite's palette.  Otherwise true colour is converted between
 *     formats with SpriteExtend's rules for filling low bits.  Ordered
 *     dither is applied when asked.  Alpha and translucency are blended
 *     arithmetically into 12 bpp and more.
 *   - The pixel is written with GCOL actions on the destination pixel's
 *     bits, with masks gating, and the kernel's ECF tables for mask plots.
 *   - A plot that changes nothing is handed to the kernel's PutSprite or
 *     PlotMask, as SpriteExtend hands it.  Such a plot is 1:1, in the same
 *     format, with no table and no blending.
 *
 * Blending onto 256 colours or fewer needs the BlendTable and InverseTable
 * modules' tables, which ROSGD has not got yet.  Those plots are refused.
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "sprext.h"

/* ---- pixel formats (compute_pixelformat, pixelformat_info) ------------------------- */

enum { PF_PAL, PF_12, PF_15, PF_16, PF_32, PF_32HI };

struct pf {
    int kind, rgb, alpha;
    uint32_t l2bpp;
};

static struct pf pixfmt(uint32_t ncolour, uint32_t flags, uint32_t l2bpp)
{
    struct pf f = { PF_PAL, (flags >> 14) & 1, (flags >> 15) & 1, l2bpp };
    if (l2bpp <= 3) {
        f.rgb = f.alpha = 0;
    } else if (l2bpp == 4) {
        if (flags & 0x80)
            f.kind = PF_16, f.alpha = 0;
        else if (ncolour < 4096)
            f.kind = PF_12;
        else
            f.kind = PF_15;
    } else {
        f.kind = PF_32;
    }
    return f;
}

/* channel i (0 R, 1 G, 2 B, 3 A): width and top bit (exclusive) */
static void chan(const struct pf *f, int i, uint32_t *bits, uint32_t *top)
{
    static const uint8_t b12[4] = { 4, 4, 4, 4 }, t12[4] = { 4, 8, 12, 16 };
    static const uint8_t b15[4] = { 5, 5, 5, 1 }, t15[4] = { 5, 10, 15, 16 };
    static const uint8_t b16[4] = { 5, 6, 5, 0 }, t16[4] = { 5, 11, 16, 16 };
    static const uint8_t b32[4] = { 8, 8, 8, 8 }, t32[4] = { 8, 16, 24, 32 };
    static const uint8_t thi[4] = { 16, 24, 32, 0 };
    const uint8_t *b, *t;
    switch (f->kind) {
    case PF_12: b = b12, t = t12; break;
    case PF_15: b = b15, t = t15; break;
    case PF_16: b = b16, t = t16; break;
    case PF_32HI: b = b32, t = thi; break;
    default: b = b32, t = t32; break;
    }
    int j = i;
    if (f->rgb && (i == 0 || i == 2))
        j = 2 - i;                              /* R and B swap places */
    *bits = b[i], *top = t[j];
    if (i == 3 && (!f->alpha || f->kind == PF_16 || f->kind == PF_32HI))
        *bits = 0;
}

static uint32_t field(uint32_t v, uint32_t bits, uint32_t top)
{
    return bits ? (v >> (top - bits)) & ((1u << bits) - 1) : 0;
}

static uint32_t rev(uint32_t v)
{
    return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
}

/* convert_pixel: between true-colour formats (and a palette entry) */
static uint32_t convert(uint32_t v, const struct pf *in, const struct pf *out)
{
    if (in->kind == out->kind && in->rgb == out->rgb && in->alpha == out->alpha)
        return v;
    if (in->kind == PF_32HI && out->kind == PF_32) {
        uint32_t r = out->rgb ? rev(v) : v >> 8;
        if (out->alpha)
            r |= 0xFF000000u;
        return r;
    }
    if (in->kind == PF_32 && out->kind == PF_32) {          /* the RGB order differs */
        uint32_t r = rev(v);
        if (in->alpha == out->alpha)
            r = r >> 8 | r << 24;                            /* ROR 8: the top byte kept */
        else
            r >>= 8;
        if (out->alpha && !in->alpha)
            r |= 0xFF000000u;
        return r;
    }
    if (in->kind == PF_15 && out->kind == PF_16 && in->rgb == out->rgb) {
        uint32_t r = (v & 0x1F) | (v >> 5) << 6 | (((v >> 5) & 0x1F) >> 4) << 5;
        if (in->alpha)
            r &= ~0x10000u;
        return r;                                           /* the bit 15 leak kept */
    }
    if (in->kind == PF_16 && out->kind == PF_15 && in->rgb == out->rgb) {
        uint32_t r = (v & 0x1F) | (v >> 6) << 5;
        return out->alpha ? r | 0x8000 : r;
    }
    /* component-wise: the top bits of each; low bits filled only where two or
     * more channels expand from the same width (and a 565 green to 8 bits) */
    uint32_t bi[3], ti[3], bo[3], to[3];
    for (int i = 0; i < 3; i++)
        chan(in, i, &bi[i], &ti[i]), chan(out, i, &bo[i], &to[i]);
    uint32_t r = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t c = field(v, bi[i], ti[i]), o;
        if (bi[i] >= bo[i]) {
            o = c >> (bi[i] - bo[i]);
        } else {
            o = c << (bo[i] - bi[i]);
            int peers = 0;
            for (int j = 0; j < 3; j++)
                if (bi[j] == bi[i] && bi[j] < bo[j])
                    peers++;
            if (peers >= 2 && 2 * bi[i] >= bo[i])
                o |= c >> (2 * bi[i] - bo[i]);
            else if (in->kind == PF_16 && i == 1 && bo[i] == 8)
                o |= c >> 4;
        }
        r |= o << (to[i] - bo[i]);
    }
    uint32_t ba, ta, boa, toa;
    chan(in, 3, &ba, &ta), chan(out, 3, &boa, &toa);
    if (boa) {
        uint32_t a;
        if (!ba)
            a = (1u << boa) - 1;
        else {
            a = field(v, ba, ta);
            if (ba == 1)
                a = a ? (1u << boa) - 1 : 0;
            else if (ba < boa)
                a = a * 17;
            else
                a >>= ba - boa;
        }
        r |= a << (toa - boa);
    }
    return r;
}

/* ---- the plot in hand ---------------------------------------------------------------- */

enum { TTR_NONE, TTR_NORMAL, TTR_WIDE, TTR_32K, TTR_MAP, TTR_PALETTE };

struct plot {
    struct sx_dest d;
    struct sx_mode m;
    uint32_t sp, mode, reason;
    struct pf in, out, idx;                 /* idx: the 32K table's index format */
    uint32_t w, h, in_l2bpc, in_bpc, in_bpp;
    uint32_t action, use_mask, masktype;   /* 0 old, 1 1bpp, 2 alpha */
    uint32_t ttr, ttr_type, *wide, map_r12, map_pc;
    int dither, blending, k;
    uint32_t tr;                            /* 256 - T */
    uint32_t ecf;                           /* the kernel's table, for mask plots */
    uint32_t leak_x;                        /* the 15->16 leak's pending pixel, or ~0 */
    int32_t group_x;                        /* the replicated group's first column */
    int trans;                              /* a transformed plot: pixels by absolute index */
    const struct sx_jsrc *j;                /* a JPEG's pixels in place of a sprite's */
};

static uint32_t src_pixel(const struct plot *p, uint32_t s, uint32_t r)
{
    if (p->j) {
        const uint8_t *row = p->j->pix + (size_t)r * p->j->stride;
        if (p->j->l2bpp == 5)
            return ((const uint32_t *)(const void *)row)[s];
        if (p->j->l2bpp == 4)
            return ((const uint16_t *)(const void *)row)[s];
        return row[s];
    }
    uint32_t stride = (ros_ld32(p->sp + SP_WIDTH) + 1) * 4;
    uint32_t bit = p->trans ? s << p->in_l2bpc : ros_ld32(p->sp + SP_LBIT) + (s << p->in_l2bpc);
    uint32_t w = ros_ld32(p->sp + ros_ld32(p->sp + SP_IMAGE) + r * stride + (bit >> 5) * 4);
    uint32_t v = p->in_bpc >= 32 ? w : (w >> (bit & 31)) & ((1u << p->in_bpc) - 1);
    return p->in_bpp >= 32 ? v : v & ((1u << p->in_bpp) - 1);
}

/* the mask at (s, r): 0 transparent; for alpha masks the byte */
static uint32_t mask_pixel(const struct plot *p, uint32_t s, uint32_t r)
{
    uint32_t base = p->sp + ros_ld32(p->sp + SP_TRANS);
    if (p->masktype == 2) {
        uint32_t stride = (p->w * 8 + 31) / 32 * 4;
        return ros_ld8(base + r * stride + s);
    }
    if (p->masktype == 1) {
        uint32_t stride = (p->w + 31) / 32 * 4;
        return (ros_ld32(base + r * stride + (s >> 5) * 4) >> (s & 31)) & 1;
    }
    uint32_t stride = (ros_ld32(p->sp + SP_WIDTH) + 1) * 4;
    uint32_t bit = p->trans ? s << p->in_l2bpc : ros_ld32(p->sp + SP_LBIT) + (s << p->in_l2bpc);
    return (ros_ld32(base + r * stride + (bit >> 5) * 4) >> (bit & 31)) & 1;
}

static uint32_t *dest_word(const struct plot *p, int32_t x, int32_t y, uint32_t *sh)
{
    uint32_t bit = (uint32_t)x << p->d.l2bpc;
    *sh = bit & 31;
    return ros_ptr(p->d.screen + p->d.linelen * (p->d.ywind - (uint32_t)y) + (bit >> 5) * 4);
}

/* ---- ordered dither (add_ordered_dither) ------------------------------------------------ */

static uint32_t dither(const struct plot *p, uint32_t v, const struct pf *f, int32_t x, int32_t y)
{
    uint32_t d = ((uint32_t)y & 1) | ((((uint32_t)x ^ (uint32_t)y) & 1) << 1);
    if (p->d.ecf_yoffset & 1)
        d ^= 3;
    if (p->d.ecf_shift & p->d.bpp)
        d ^= 2;
    uint32_t D = d << (23 + p->k);
    for (int i = 2; i >= 0; i--) {
        uint32_t bits, top;
        chan(f, i, &bits, &top);
        uint32_t off = top - bits, xs = 32 - bits - off;
        uint64_t sum = (uint64_t)((v << xs) & 0xFFFFFFFFu) + D;
        if (sum < (1ull << 32))
            v += D >> xs;
    }
    return v;
}

/* ---- translation (preparettr, apply_ttr) --------------------------------------------------- */

static uint32_t gcol_word(uint32_t g, const struct sx_dest *d)
{
    uint32_t t = g & 3, t0 = t & 1, ch[3] = { (g >> 2) & 3, (g >> 4) & 3, (g >> 6) & 3 };
    int rgb = (d->flags >> 14) & 1, alpha = (d->flags >> 15) & 1;
    uint32_t v, c[3];
    if (d->bpp == 32) {
        for (int i = 0; i < 3; i++)
            c[i] = ((ch[i] << 2) | t) * 0x11;
        v = rgb ? c[2] | c[1] << 8 | c[0] << 16 : c[0] | c[1] << 8 | c[2] << 16;
        return alpha ? v | 0xFF000000u : v;
    }
    if (!(d->ncolour & 4096)) {                            /* 4444 */
        for (int i = 0; i < 3; i++)
            c[i] = (ch[i] << 2) | t;
        v = rgb ? c[2] | c[1] << 4 | c[0] << 8 : c[0] | c[1] << 4 | c[2] << 8;
        return alpha ? v | 0xF000 : v;
    }
    for (int i = 0; i < 3; i++)
        c[i] = (ch[i] << 3) | (t << 1) | t0;
    if (d->flags & 0x80) {                                 /* 565 */
        uint32_t g6 = (ch[1] << 4) | (t << 2) | (t0 << 1) | t0;
        return rgb ? c[2] | g6 << 5 | c[0] << 11 : c[0] | g6 << 5 | c[2] << 11;
    }
    v = rgb ? c[2] | c[1] << 5 | c[0] << 10 : c[0] | c[1] << 5 | c[2] << 10;
    return alpha ? v | 0x8000 : v;
}

static uint32_t pal16_word(uint32_t P, const struct sx_dest *d)
{
    uint32_t r = (P >> 8) & 255, g = (P >> 16) & 255, b = P >> 24;
    int rgb = (d->flags >> 14) & 1, alpha = (d->flags >> 15) & 1;
    if ((d->ncolour >> 12) == 0) {
        uint32_t v = rgb ? b >> 4 | (g >> 4) << 4 | (r >> 4) << 8 : r >> 4 | (g >> 4) << 4 | (b >> 4) << 8;
        return alpha ? v | 0xF000 : v;
    }
    if (d->flags & 0x80)
        return rgb ? b >> 3 | (g >> 2) << 5 | (r >> 3) << 11 : r >> 3 | (g >> 2) << 5 | (b >> 3) << 11;
    uint32_t v = rgb ? b >> 3 | (g >> 3) << 5 | (r >> 3) << 10 : r >> 3 | (g >> 3) << 5 | (b >> 3) << 10;
    return alpha ? v | 0x8000 : v;
}

static os_error *prepare_ttr(struct plot *p, uint32_t *flags2, uint32_t r7)
{
    p->ttr_type = TTR_NONE;
    if (p->reason == 50 || p->reason == 51 || p->reason == 55)
        return NULL;
    uint32_t img = ros_ld32(p->sp + SP_IMAGE), trn = ros_ld32(p->sp + SP_TRANS);
    uint32_t lo = img < trn ? img : trn, entries = (lo - 44) >> 3;
    uint32_t nsrc = p->in_bpp >= 32 ? 0 : 1u << p->in_bpp;
    if (p->m.l2bpp <= 3) {
        if (*flags2 & 8)
            return sx_err(SX_BADTRAN);
        int optional = (*flags2 & 1) && img != 44 && trn != 44 && entries == nsrc;
        if (optional && p->d.bpp >= 16) {
            p->wide = malloc(256 * 4);
            if (p->d.bpp == 16) {
                for (uint32_t i = 0; i < nsrc; i++)
                    p->wide[i] = pal16_word(ros_ld32(p->sp + 44 + 8 * i), &p->d);
                p->ttr_type = TTR_WIDE;
            } else {
                for (uint32_t i = 0; i < nsrc; i++)
                    p->wide[i] = ros_ld32(p->sp + 44 + 8 * i);
                p->ttr_type = TTR_PALETTE;
            }
            return NULL;
        }
        if (p->d.bpp <= 8) {
            if (!r7) {
                if (p->in_bpp != p->d.bpp)
                    return sx_err(SX_BADTRAN);
                if (!optional)
                    *flags2 &= ~0xBu;
                return NULL;
            }
            int ident = p->in_bpp == p->d.bpp;
            for (uint32_t i = 0; i < nsrc; i++) {
                uint32_t v = ros_ld8(r7 + i);
                if (v >= (1u << p->d.bpp))
                    return sx_err(SX_BADTRAN);
                if (v != i)
                    ident = 0;
            }
            if (ident) {
                *flags2 &= ~0xBu;
                return NULL;
            }
            p->ttr = r7, p->ttr_type = TTR_NORMAL;
            return NULL;
        }
        if (!r7)
            return sx_err(SX_BADTRAN);
        p->wide = malloc(256 * 4);
        for (uint32_t i = 0; i < nsrc; i++) {
            if (*flags2 & 2)
                p->wide[i] = p->d.bpp == 16 ? ros_ld16(r7 + 2 * i) : ros_ld32(r7 + 4 * i);
            else
                p->wide[i] = gcol_word(ros_ld8(r7 + i), &p->d);
        }
        p->ttr_type = TTR_WIDE;
        return NULL;
    }
    if (*flags2 & 8) {
        p->ttr_type = TTR_MAP;
        p->map_r12 = ros_ld32(r7), p->map_pc = ros_ld32(r7 + 4);
        return NULL;
    }
    if (p->d.bpp >= 16 || !r7) {
        /* No table.  For a sprite (not JPEG) the original takes the "none"
         * path here, even onto 256 colours or fewer.  Its code generator
         * then asserts, and the plot ends quietly (see sx_scaled) */
        *flags2 &= ~0xBu;
        return NULL;
    }
    uint32_t w0 = ros_ld32(r7);
    if ((w0 != 0x2E4B3233u && w0 != 0x2B4B3233u) || ros_ld32(r7 + 8) != w0)
        return sx_err(SX_BADTRAN);
    uint32_t t = ros_ld32(r7 + 4), sflags = p->m.flags & ~0x8000u;
    if (!(p->m.l2bpp == 5 && w0 == 0x2E4B3233u)) {
        if (w0 == 0x2E4B3233u) {
            if (p->m.ncolour != 65535 || sflags)
                return sx_err(SX_BADTRAN);
        }
        /* "32K+": its header is taken on trust here (ColourTrans's layout) */
    }
    p->ttr = t, p->ttr_type = TTR_32K;
    return NULL;
}

/* ---- blending (blend_rgb) ------------------------------------------------------------------- */

static uint32_t blend(uint32_t P, const struct pf *in, uint32_t D, const struct pf *S,
                      const struct pf *B, uint32_t a, int AT)
{
    int t = B->rgb ? 2 : 0;
    uint32_t bits, tops[3], tb0;
    for (int i = 0; i < 3; i++)
        chan(in, i, &bits, &tops[i]);
    chan(B, t, &bits, &tb0);
    uint32_t topmost = tops[0] > tops[2] ? tops[0] : tops[2];
    if (tb0 > topmost)
        topmost = tb0;
    if ((int)topmost + AT > 32)
        a >>= (AT - 8), AT = 8;
    uint32_t ia = (1u << AT) - a, out = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t bs, ts, bb, tb, bd, td;
        chan(in, i, &bs, &ts), chan(B, i, &bb, &tb), chan(S, i, &bd, &td);
        uint32_t T = (P & (((1u << bs) - 1) << (ts - bs))) * a;
        if (bs < bb)
            T += T >> bs;
        uint32_t f = (T >> (ts + (uint32_t)AT - bb)) & ((1u << bb) - 1);
        if (ia) {
            uint32_t sh = td + (uint32_t)AT - bb;
            uint32_t U = (D & (((1u << bd) - 1) << (td - bd))) * ia;
            if (bd < bb)
                U += U >> bd;
            uint32_t m = (1u << bb) - 1;
            if (i != t && bb == 8 && (sh & 1))
                m &= ~1u;
            f += (U >> sh) & m;
        }
        out |= f << (tb - bb);
    }
    if (B->alpha) {
        uint32_t ab, at;
        chan(B, 3, &ab, &at);
        if (ab)
            out |= ((1u << ab) - 1) << (at - ab);
    }
    return out;
}

/* ---- one destination pixel ------------------------------------------------------------------ */

static void write_bits(uint32_t *w, uint32_t sh, uint32_t width, uint32_t action, uint32_t v)
{
    uint32_t mask = width >= 32 ? ~0u : ((1u << width) - 1) << sh;
    uint32_t old = *w, nv = width >= 32 ? v : (v << sh) & mask, r;
    switch (action) {
    case 0: r = (old & ~mask) | nv; break;
    case 1: r = old | nv; break;
    case 2: r = old & (nv | ~mask); break;
    case 3: r = old ^ nv; break;
    case 4: r = old ^ mask; break;
    case 5: r = old; break;
    case 6: r = old & ~nv; break;
    default: r = old | (nv ^ mask); break;
    }
    *w = r;
}

/* One destination pixel.  With *reuse set, the value computed for the
 * group's first pixel is written again (the dither and blend loop's
 * replication); the value written is left in *reuse_v. */
static void plot_pixel(struct plot *p, int32_t X, int32_t Y, uint32_t s, uint32_t r,
                       int reuse, uint32_t *reuse_v)
{
    /* Outside the graphics window there is nothing to write.  The loops
     * above clip to it before they ever come here, so this costs two
     * compares and should never fire.  But dest_word turns a coordinate
     * into an address with no bound of its own.  A coordinate that slipped
     * through wrote wherever the arithmetic landed and took the machine
     * down with a data abort (#190: a manual's sprite did exactly that).
     * A sprite plotted wrongly is a sprite plotted wrongly. It must not be
     * the end of the session.
     *
     * There is one compare for each axis.  Below the low edge the
     * subtraction wraps to a huge unsigned number, so a single unsigned
     * test catches both ends.  The two results are ORed, and not
     * short-circuited, to leave one branch.  That branch predicts
     * perfectly, because it is taken for no pixel of a plot that was
     * clipped properly. */
    if (((uint32_t)(X - p->d.gwl) > (uint32_t)(p->d.gwr - p->d.gwl)) |
        ((uint32_t)(Y - p->d.gwb) > (uint32_t)(p->d.gwt - p->d.gwb)))
        return;
    uint32_t sh, *w = dest_word(p, X, Y, &sh), bpc = p->d.bpc, bpp = p->d.bpp;
    if (reuse && !p->blending) {
        if (p->use_mask && !mask_pixel(p, s, r))
            return;
        write_bits(w, sh, bpc, p->action, *reuse_v);
        return;
    }
    /* blending, the value is blended with each pixel's own destination,
     * but dithered in the phase of the group's first pixel */
    int32_t DX = reuse ? p->group_x : X;
    if (!reuse)
        p->group_x = X;
    uint32_t mval = 1;
    if (p->use_mask) {
        mval = mask_pixel(p, s, r);
        if (!mval)
            return;
    }
    if (p->reason == 50 || p->reason == 51 || p->reason == 55) {
        const uint32_t *t = ros_ptr(p->ecf + 8 * ((p->d.ywind - (uint32_t)Y) & 7));
        uint32_t m = bpp >= 32 ? ~0u : ((1u << bpp) - 1) << sh;
        *w = (*w | (t[0] & m)) ^ (t[1] & m);
        return;
    }
    uint32_t v = src_pixel(p, s, r);
    uint32_t oldpix = bpc >= 32 ? *w : (*w >> sh) & ((1u << bpc) - 1);
    struct pf cur = p->in;
    int dithered = 0;
    if ((p->blending & 2) && p->masktype != 2 && p->in.alpha) {
        uint32_t ab, at;
        chan(&p->in, 3, &ab, &at);
        if (ab && !field(v, ab, at)) {
            v = oldpix;
            goto finish;
        }
    }
    switch (p->ttr_type) {
    case TTR_NORMAL: v = ros_ld8(p->ttr + v); cur.kind = PF_PAL; break;
    case TTR_WIDE: v = p->wide[v & 255]; cur = p->out; break;
    case TTR_PALETTE: v = p->wide[v & 255]; cur.kind = PF_32HI, cur.rgb = cur.alpha = 0; break;
    case TTR_32K:
        if (p->dither)
            v = dither(p, v, &cur, DX, Y), dithered = 1;
        v = ros_ld8(p->ttr + convert(v, &cur, &p->idx));
        cur.kind = PF_PAL;
        break;
    case TTR_MAP: {
        struct pf f32 = { PF_32, 0, cur.alpha && p->out.alpha, 5 };
        uint32_t c = convert(v, &cur, &f32) << 8;
        struct ros_cpu cc;
        ros_cpu_enter(&cc);
        cc.r[0] = c, cc.r[12] = p->map_r12;
        ros_call(&cc, p->map_pc);
        v = cc.r[0] >> 8;
        cur = f32;
        cur.alpha = 0;
        break;
    }
    default: break;
    }
    if (p->blending && cur.kind != PF_PAL && p->out.kind != PF_PAL) {
        int AT = 8;
        uint32_t al;
        if (p->blending & 2) {
            uint32_t m8 = p->masktype == 2 ? mval : 0;
            if (p->masktype != 2) {
                uint32_t ab, at;
                chan(&p->in, 3, &ab, &at);
                uint32_t raw = field(src_pixel(p, s, r), ab, at);
                if (ab == 8)
                    m8 = raw;
                else if (ab == 4) {
                    AT = (p->blending & 1) ? 24 : 16;
                    al = (p->blending & 1) ? ((raw + (raw >= 8)) << 12) * p->tr
                                           : (raw * 17 + (raw >= 8)) << 8;
                    goto have;
                } else {                        /* 1 bit */
                    if (!(p->blending & 1)) {
                        v = convert(v, &cur, &p->out);
                        cur = p->out;
                        goto converted;
                    }
                    al = p->tr;
                    goto have;
                }
            }
            al = m8 + (m8 >= 128);
            if (p->blending & 1)
                al *= p->tr, AT = 16;
        } else {
            al = p->tr;
        }
    have:;
        struct pf B = p->out;
        if (p->dither) {
            B.kind = PF_32, B.l2bpp = 5;
        }
        if (cur.kind == PF_32HI) {
            v >>= 8;
            cur.kind = PF_32, cur.rgb = 0, cur.alpha = 0;
        }
        v = blend(v, &cur, oldpix, &p->out, &B, al, AT);
        cur = B;
    }
converted:
    if (p->dither && !dithered && cur.kind != PF_PAL)
        v = dither(p, v, &cur, DX, Y);
    if (p->j && p->j->grey && cur.kind != PF_PAL && p->out.kind == PF_PAL) {
        /* JPEG's 24-bit grey with no table (below 4bpp): red's top bits,
         * inverted, as the default Wimp palette has it (convert_pixel) */
        v = ~(v >> (8 - bpp)) & ((1u << bpp) - 1);
        cur.kind = PF_PAL;
    }
    if (cur.kind != PF_PAL && p->out.kind != PF_PAL)
        v = convert(v, &cur, &p->out);
finish:
    /* bpc and bpp reach 32, where a shift of 32 is undefined.  At 32 there
     * are no bits above the container to set, and nothing to replicate into
     * (clang's analyser: core.BitwiseShift) */
    if (p->action == 2 && bpp != 32 && bpc < 32)
        v |= ~((1u << bpc) - 1);
    if (bpc > bpp && bpp < 32)
        v |= v << bpp;
    /* the 15->16 leak: bit 16 of an even pixel's value reaches bit 0 of the
     * next pixel in the same word, through the same GCOL operation */
    uint32_t leak = p->out.kind == PF_16 && p->in.kind == PF_15 && !p->in.alpha &&
                    p->ttr_type == TTR_NONE && (v & 0x10000) && sh == 0;
    if (p->leak_x == (uint32_t)X && p->action == 0 && !p->use_mask && !p->blending)
        v |= 1;
    write_bits(w, sh, bpc, p->action, v);
    *reuse_v = v;
    if (leak) {
        write_bits(w, 16, 1, p->action, 1);
        p->leak_x = (uint32_t)X + 1;
    }
}

/* ---- 52 and 50 ------------------------------------------------------------------------------ */

static uint32_t udiv(uint64_t a, uint32_t b)
{
    return b ? (uint32_t)(a / b) : 0;
}

os_error *sx_scaled(struct ros_cpu *s, uint32_t reason)
{
    struct plot p;
    memset(&p, 0, sizeof p);
    p.reason = reason;
    p.leak_x = ~0u;
    uint32_t r5 = s->r[5], r7 = s->r[7];
    if (reason == 50)
        r5 = 8, r7 = 0;
    uint32_t flags2 = r5 >> 4;
    if (flags2 & ~0xFFFu)
        return sx_err(SX_BADFLAGS);
    r5 &= 15;
    p.dither = (flags2 & 4) != 0;
    p.blending = (flags2 & 0xFF0) ? 1 : 0;
    p.tr = 256 - ((flags2 >> 4) & 0xFF);
    struct sx_call c;
    os_error *e = sx_find(s, &c, 0);
    if (e)
        return e;
    p.sp = c.sp, p.mode = ros_ld32(c.sp + SP_MODE);
    sx_read_dest(&p.d);
    if (sx_read_mode(p.mode, &p.m))
        return sx_err(SX_BADMODE);
    int has_mask = ros_ld32(p.sp + SP_TRANS) != ros_ld32(p.sp + SP_IMAGE);
    if (((p.mode & 0x80000000u) || (p.m.flags & 0x8000)) && (r5 & 8) && reason != 50)
        p.blending |= 2;
    if (p.blending)
        r5 &= ~7u;
    if (!has_mask)
        r5 &= ~8u;
    p.action = r5 & 7;
    p.use_mask = (r5 & 8) != 0;
    p.masktype = p.mode & 0x80000000u ? 2 : (p.mode >> 27) ? 1 : 0;
    p.in_l2bpc = p.m.l2bpc, p.in_bpc = 1u << p.m.l2bpc, p.in_bpp = 1u << p.m.l2bpp;
    p.in = pixfmt(p.m.ncolour, p.m.flags, p.m.l2bpp);
    p.out = pixfmt(p.d.ncolour, p.d.flags, p.d.l2bpp);
    p.idx = p.m.l2bpp == 5 ? (struct pf){ PF_15, 0, 0, 4 } : p.in;
    p.idx.alpha = 0;
    p.ecf = reason == 51 ? p.d.fg_ecf : p.d.bg_ecf;
    p.w = ((ros_ld32(p.sp + SP_WIDTH) * 32) + (ros_ld32(p.sp + SP_RBIT) - ros_ld32(p.sp + SP_LBIT) + 1)) >>
          p.m.l2bpc;
    p.h = ros_ld32(p.sp + SP_HEIGHT) + 1;

    uint32_t xmag = 1, ymag = 1, xdiv = 1, ydiv = 1;
    if (s->r[6]) {
        xmag = ros_ld32(s->r[6]), ymag = ros_ld32(s->r[6] + 4);
        xdiv = ros_ld32(s->r[6] + 8), ydiv = ros_ld32(s->r[6] + 12);
        if (xdiv && xmag % xdiv == 0)
            xmag /= xdiv, xdiv = 1;
        if (ydiv && ymag % ydiv == 0)
            ymag /= ydiv, ydiv = 1;
    }
    if ((e = prepare_ttr(&p, &flags2, r7)) != NULL)
        goto out;
    if (p.in.kind != PF_PAL && p.out.kind == PF_PAL && p.ttr_type == TTR_NONE &&
        reason == 52) {
        /* true colour onto a palette with no table: convert_pixel's
         * assert(is_it_jpeg) fails, and SpriteExtend returns V clear having
         * plotted nothing (c/asmcore 2537; Sources/PutScaled exit_erl) */
        goto out;
    }
    if (p.in.kind != PF_PAL && p.m.l2bpp == 4 && p.out.kind != PF_PAL && p.d.l2bpp == 4 &&
        (p.blending & 1) && p.use_mask && p.dither) {
        /* A 16 bpp sprite, translucent, masked and dithered, onto 16 bpp:
         * the original's generated routine does not fit its 256-word
         * buffer, and the plot ends quietly (Sources/PutScaled exit_erl).
         * Inferred from the 32-bit system, which plots nothing here while
         * plotting every lesser combination. */
        goto out;
    }
    if (p.blending && p.out.kind == PF_PAL) {
        e = ros_error(0x1E6, "SpriteExtend: blending onto 256 colours or fewer needs BlendTable");
        goto out;
    }

    /* the hand-off: a 1:1 plot that SpriteExtend would not change */
    int typ = (p.mode >> 27) & 15;
    if (typ == 15)
        typ = (int)((p.mode >> 20) & 127);
    int handoff = p.ttr_type == TTR_NONE && p.in_bpp == p.d.bpp && p.m.l2bpc == p.d.l2bpc &&
                  xmag == xdiv && ymag == ydiv && !(flags2 & 0xFFF) && !p.blending &&
                  (!p.use_mask || (!(typ & 0xFE) && !(p.mode & 0x80000000u)));
    if (handoff && p.d.l2bpp >= 2)
        handoff = p.m.ncolour == p.d.ncolour && !((p.m.flags ^ p.d.flags) & 0xF280u);
    if (handoff && reason != 51) {
        uint32_t r[10] = { (c.code & ~0xFFu) | (reason == 52 ? 34 : 49), s->r[1], s->r[2],
                           s->r[3], s->r[4], r5 };
        sx_swi(XOS_SpriteOp, r, &e);
        goto out;
    }
    if (!xmag || !ymag || !xdiv || !ydiv) {
        e = sx_err(SX_DIVZERO);
        goto out;
    }

    /* ordered dither */
    if (p.dither && reason == 52 && p.m.l2bpp >= 4 &&
        (p.d.l2bpp < p.m.l2bpp || (p.d.l2bpp == p.m.l2bpp && p.blending && p.d.l2bpp < 5))) {
        if (p.d.bpp >= 16) {
            uint32_t rb, rt;
            chan(&p.out, 0, &rb, &rt);
            p.k = 7 - (int)rb;
        } else {
            p.k = p.d.bpp == 8 ? 4 : 6 - (int)p.d.l2bpp;
        }
    } else {
        p.dither = 0;
    }

    /* coordinates, clipping, the DDA's start */
    int32_t X0 = ((int32_t)s->r[3] + p.d.orgx) >> p.d.xeig;
    int32_t Y0 = ((int32_t)s->r[4] + p.d.orgy) >> p.d.yeig;
    int32_t skipx = p.d.gwl - X0 > 0 ? p.d.gwl - X0 : 0;
    int32_t nx = (int32_t)udiv((uint64_t)(uint32_t)(p.w * xmag), xdiv);
    if (nx > p.d.gwr + 1 - X0)
        nx = p.d.gwr + 1 - X0;
    int32_t xsize = nx - skipx;
    int32_t skipy = p.d.gwb - Y0 > 0 ? p.d.gwb - Y0 : 0;
    int32_t ny = (int32_t)udiv((uint64_t)(uint32_t)(p.h * ymag), ydiv);
    if (ny > p.d.gwt + 1 - Y0)
        ny = p.d.gwt + 1 - Y0;
    int32_t ysize = ny - skipy;
    if (xsize <= 0 || ysize <= 0)
        goto out;
    uint32_t xcount = xmag - (uint32_t)(((uint64_t)(uint32_t)skipx * xdiv) % xmag);
    uint32_t s0 = (uint32_t)(((uint64_t)(uint32_t)skipx * xdiv) / xmag);
    int quirk = (p.dither || p.blending) && xmag > xdiv && xcount < xdiv;
    for (int32_t j = 0; j < ysize; j++) {
        int32_t Y = Y0 + skipy + j, E = Y - Y0;
        uint32_t t = (uint32_t)(((uint64_t)(uint32_t)(E + 1) * ydiv - 1) / ymag);
        uint32_t row = p.h - 1 - t;
        p.leak_x = ~0u;
        uint32_t value = 0;
        for (int32_t i = 0; i < xsize; i++) {
            int32_t X = X0 + skipx + i, D = X - X0;
            uint32_t sc = (uint32_t)(((uint64_t)(uint32_t)(D + 1) * xdiv - 1) / xmag);
            if (quirk && i == 0)
                sc = s0;
            plot_pixel(&p, X, Y, sc, row, 0, &value);
        }
    }
out:
    free(p.wide);
    return e;
}

/* ---- JPEG_PlotScaled (jpeg.c) --------------------------------------------------------------------- */

/* The original enters its sprite code at putsprscaled_frompjs with a fake
 * 32bpp sprite: plot action 0, no mask, the scale rationalised as there, no
 * hand-off to the kernel.  The pixels are build()'s, made once the geometry
 * is known. */
os_error *sx_scaled_jpeg(struct sx_jsrc *j)
{
    struct plot p;
    memset(&p, 0, sizeof p);
    p.reason = 52;
    p.leak_x = ~0u;
    p.j = j;
    sx_read_dest(&p.d);
    p.out = pixfmt(p.d.ncolour, p.d.flags, p.d.l2bpp);
    p.blending = (j->flags & 0xFF0) ? 1 : 0;
    p.tr = 256 - ((j->flags >> 4) & 0xFF);
    p.w = j->w, p.h = j->h;

    uint32_t xmag = j->scale[0], ymag = j->scale[1], xdiv = j->scale[2], ydiv = j->scale[3];
    if (xdiv && xmag % xdiv == 0)
        xmag /= xdiv, xdiv = 1;
    if (ydiv && ymag % ydiv == 0)
        ymag /= ydiv, ydiv = 1;
    if (!xdiv || !ydiv)
        return sx_err(SX_DIVZERO);

    int32_t X0 = (j->x + p.d.orgx) >> p.d.xeig;
    int32_t Y0 = (j->y + p.d.orgy) >> p.d.yeig;
    int32_t skipx = p.d.gwl - X0 > 0 ? p.d.gwl - X0 : 0;
    int32_t nx = (int32_t)udiv((uint64_t)(uint32_t)(p.w * xmag), xdiv);
    if (nx > p.d.gwr + 1 - X0)
        nx = p.d.gwr + 1 - X0;
    int32_t xsize = nx - skipx;
    if (xsize <= 0)
        return NULL;
    int32_t skipy = p.d.gwb - Y0 > 0 ? p.d.gwb - Y0 : 0;
    int32_t ny = (int32_t)udiv((uint64_t)(uint32_t)(p.h * ymag), ydiv);
    if (ny > p.d.gwt + 1 - Y0)
        ny = p.d.gwt + 1 - Y0;
    int32_t ysize = ny - skipy;
    if (ysize <= 0)
        return NULL;
    if (!xmag || !ymag)
        return sx_err(SX_DIVZERO);
    uint32_t s0 = (uint32_t)(((uint64_t)(uint32_t)skipx * xdiv) / xmag);
    uint32_t xcount = xmag - (uint32_t)(((uint64_t)(uint32_t)skipx * xdiv) % xmag);

    struct sx_jgeom g = { &p.d, s0, (uint32_t)(((uint64_t)(uint32_t)skipy * ydiv) / ymag),
                          (uint32_t)xsize, (uint32_t)ysize, xmag, ymag, xdiv, ydiv };
    os_error *e = j->build(j, &g);
    if (e)
        return e;

    if (j->kind == SX_JNATIVE) {
        p.in = p.out;
    } else {
        p.in = (struct pf){ PF_32, 0, 0, 5 };
    }
    p.in_l2bpc = j->l2bpp, p.in_bpc = 1u << j->l2bpp, p.in_bpp = 1u << j->l2bpp;
    p.m.l2bpp = j->l2bpp, p.m.l2bpc = j->l2bpp;
    p.idx = (struct pf){ PF_15, 0, 0, 4 };
    if (j->flags & 8) {
        p.ttr_type = TTR_MAP;
        p.map_r12 = ros_ld32(j->map), p.map_pc = ros_ld32(j->map + 4);
    } else if (j->ttr && j->kind != SX_JNATIVE) {
        p.ttr_type = TTR_32K, p.ttr = j->ttr;
    }
    if (p.blending && p.out.kind == PF_PAL)
        return ros_error(0x1E6, "SpriteExtend: blending onto 256 colours or fewer needs BlendTable");

    /* ordered dither (find_or_compile_code, loop_y) */
    uint32_t dt = j->dither_tc;
    if ((dt & 1) && j->l2bpp >= 4 && !(dt & 2) &&
        (p.d.l2bpp < j->l2bpp || (p.d.l2bpp == j->l2bpp && p.blending && p.d.l2bpp < 5))) {
        p.dither = 1;
        if (p.d.bpp >= 16) {
            uint32_t rb, rt;
            chan(&p.out, 0, &rb, &rt);
            p.k = 7 - (int)rb;
        } else if (p.d.bpp == 8) {
            p.k = j->grey_jpeg ? 3 : 4;
        } else {
            p.k = 6 - (int)p.d.l2bpp;
        }
    }

    int quirk = (p.dither || p.blending) && xmag > xdiv && xcount < xdiv;
    for (int32_t jy = 0; jy < ysize; jy++) {
        int32_t Y = Y0 + skipy + jy, E = Y - Y0;
        uint32_t t = (uint32_t)(((uint64_t)(uint32_t)(E + 1) * ydiv - 1) / ymag);
        uint32_t row = p.h - 1 - t;
        p.leak_x = ~0u;
        uint32_t value = 0;
        for (int32_t i = 0; i < xsize; i++) {
            int32_t X = X0 + skipx + i, D = X - X0;
            uint32_t sc = (uint32_t)(((uint64_t)(uint32_t)(D + 1) * xdiv - 1) / xmag);
            if (quirk && i == 0)
                sc = s0;
            plot_pixel(&p, X, Y, sc, row, 0, &value);
        }
    }
    return NULL;
}

/* ---- 51 PaintCharScaled ----------------------------------------------------------------------- */

os_error *sx_paint_char(struct ros_cpu *s)
{
    uint8_t *blk = ros_rma_alloc(16 + 44 + 64 + 16);
    if (!blk)
        return sx_err(SX_NOTENOUGHROOM);
    memset(blk, 0, 16 + 44 + 64 + 16);
    uint32_t b = ros_addr(blk), area = b, sp = b + 16, def = b + 16 + 44 + 64;
    ros_st8(def, (uint8_t)s->r[1]);
    uint32_t r[10] = { 10, def };
    os_error *e;
    sx_swi(XOS_Word, r, &e);
    ros_st32(area, 16 + 44 + 64), ros_st32(area + 4, 1), ros_st32(area + 8, 16);
    ros_st32(area + 12, 16 + 44 + 64);
    ros_st32(sp + SP_NEXT, 44 + 64);
    ros_st32(sp + SP_WIDTH, 0), ros_st32(sp + SP_HEIGHT, 7);
    ros_st32(sp + SP_LBIT, 0), ros_st32(sp + SP_RBIT, 7);
    ros_st32(sp + SP_IMAGE, 44), ros_st32(sp + SP_TRANS, 44 + 32), ros_st32(sp + SP_MODE, 0);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t d = ros_ld8(def + 1 + i), m = 0;
        for (uint32_t k = 0; k < 8; k++)
            if (d & (0x80u >> k))
                m |= 1u << k;
        ros_st32(sp + 44 + 32 + 4 * i, m);
    }
    struct ros_cpu c = *s;
    c.r[0] = 0x200 + 51, c.r[1] = area, c.r[2] = sp, c.r[5] = 8, c.r[7] = 0;
    e = sx_scaled(&c, 51);
    ros_rma_free(blk);
    return e;
}

/* ---- 65 TileSpriteScaled ---------------------------------------------------------------------- */

static int32_t srem(int32_t a, int32_t b)
{
    return b ? a % b : 0;
}

os_error *sx_tile(struct ros_cpu *s)
{
    struct sx_call c;
    os_error *e = sx_find(s, &c, 0);
    if (e)
        return e;
    struct sx_dest d;
    sx_read_dest(&d);
    uint32_t r[10] = { 0x200 + 40, c.area, c.sp };
    sx_swi(XOS_SpriteOp, r, &e);
    if (e)
        return e;
    int32_t w = (int32_t)r[3], h = (int32_t)r[4];
    if (s->r[6]) {
        uint32_t xm = ros_ld32(s->r[6]), ym = ros_ld32(s->r[6] + 4);
        uint32_t xd = ros_ld32(s->r[6] + 8), yd = ros_ld32(s->r[6] + 12);
        h = (int32_t)udiv((uint64_t)(uint32_t)h * ym, yd);
        if (h <= 0)
            h = 1;
        w = (int32_t)udiv((uint64_t)(uint32_t)w * xm, xd);
        if (w <= 0)
            w = 1;
    }
    int32_t tw = w << d.xeig, th = h << d.yeig;
    int32_t gx0 = d.gwl << d.xeig, gy0 = d.gwb << d.yeig;
    int32_t gx1 = (d.gwr + 1) << d.xeig, gy1 = (d.gwt + 1) << d.yeig;
    int32_t x0 = gx0 + srem((int32_t)s->r[3] - gx0, tw);
    if (x0 > gx0)
        x0 -= tw;
    int32_t y0 = gy0 + srem((int32_t)s->r[4] - gy0, th);
    if (y0 > gy0)
        y0 -= th;
    for (int32_t y = y0; y < gy1; y += th)
        for (int32_t x = x0; x < gx1; x += tw) {
            uint32_t t[10] = { 0x200 + 52, c.area, c.sp, (uint32_t)x, (uint32_t)y, s->r[5],
                               s->r[6], s->r[7] };
            sx_swi(XOS_SpriteOp, t, &e);
            if (e)
                return e;
        }
    return NULL;
}

/* ---- 55 PlotMaskTransformed, 56 PutSpriteTransformed ----------------------------------------- */

/* Draw's point transform (DrawMod DrProcess): 16.16 matrix, round half up */
static int32_t draw_x(const int32_t m[6], int32_t x, int32_t y, int which)
{
    int64_t v = which ? (int64_t)m[1] * x + (int64_t)m[3] * y : (int64_t)m[0] * x + (int64_t)m[2] * y;
    return (int32_t)((v >> 16) + ((v >> 15) & 1)) + (which ? m[5] : m[4]);
}

/* produce_increment: S * (d/256) / D in 16.16, the original's way */
static uint32_t produce_increment(int32_t d, int64_t inv, int32_t S)
{
    int neg = (inv < 0) != (d < 0);
    uint64_t A = inv < 0 ? (uint64_t)-inv : (uint64_t)inv;
    uint32_t a = d < 0 ? (uint32_t)-(int64_t)d : (uint32_t)d;
    if (neg)
        S = -S;
    uint64_t Q = (uint64_t)(((unsigned __int128)a * A) >> 16);
    uint32_t q[4] = { (uint32_t)(Q & 0xFFFF), (uint32_t)(Q >> 16 & 0xFFFF),
                      (uint32_t)(Q >> 32 & 0xFFFF), (uint32_t)(Q >> 48 & 0xFFFF) };
    int32_t t[4];
    for (int i = 0; i < 4; i++)
        t[i] = (int32_t)(q[i] * (uint32_t)S);
    uint32_t inc = (uint32_t)(t[0] >> 24) + (uint32_t)(t[1] >> 8) + ((uint32_t)t[2] << 8) +
                   ((uint32_t)t[3] << 24);
    if (t[1] & 0x80)
        inc += 1;
    return inc;
}

/* The first pixel whose centre lies right of an edge at a row's centre
 * yc: the smallest q with 256q+128 > x_edge(yc) */
static int32_t crossing(int64_t xlo, int64_t ylo, int64_t xhi, int64_t yhi, int64_t yc)
{
    int64_t D = yhi - ylo;
    int64_t N = (xlo - 128) * D + (xhi - xlo) * (yc - ylo);
    int64_t den = 256 * D;
    int64_t q = N >= 0 ? N / den : -((-N + den - 1) / den);
    return (int32_t)(q + 1);
}

os_error *sx_transformed(struct ros_cpu *s, uint32_t reason)
{
    struct plot p;
    memset(&p, 0, sizeof p);
    p.reason = reason;
    p.trans = 1;
    p.leak_x = ~0u;
    uint32_t r5 = s->r[5], r7 = s->r[7], r3 = s->r[3];
    if (reason == 55)
        r5 = 8, r7 = 0;
    uint32_t flags2 = r5 >> 4;
    if (flags2 & ~0xFFFu)
        return sx_err(SX_BADFLAGS);
    r5 &= 15;
    if (r3 & ~3u)
        return sx_err(SX_BADFLAGS);
    p.blending = (flags2 & 0xFF0) ? 1 : 0;
    p.tr = 256 - ((flags2 >> 4) & 0xFF);
    struct sx_call c;
    os_error *e = sx_find(s, &c, 0);
    if (e)
        return e;
    p.sp = c.sp, p.mode = ros_ld32(c.sp + SP_MODE);
    sx_read_dest(&p.d);
    if (sx_read_mode(p.mode, &p.m))
        return sx_err(SX_BADMODE);
    if (((p.mode & 0x80000000u) || (p.m.flags & 0x8000)) && (r5 & 8) && reason != 55)
        p.blending |= 2;
    if (p.blending)
        r5 &= ~7u;
    if (ros_ld32(p.sp + SP_TRANS) == ros_ld32(p.sp + SP_IMAGE))
        r5 &= ~8u;
    if (r7 == 0xFFFFFFFFu)
        r7 = 0;
    p.action = r5 & 7;
    p.use_mask = (r5 & 8) != 0;
    p.masktype = p.mode & 0x80000000u ? 2 : (p.mode >> 27) ? 1 : 0;
    uint32_t l2 = p.m.l2bpp;
    p.in_l2bpc = l2, p.in_bpc = 1u << l2, p.in_bpp = 1u << l2;
    p.in = pixfmt(p.m.ncolour, p.m.flags, p.m.l2bpp);
    p.out = pixfmt(p.d.ncolour, p.d.flags, p.d.l2bpp);
    p.idx = p.m.l2bpp == 5 ? (struct pf){ PF_15, 0, 0, 4 } : p.in;
    p.idx.alpha = 0;
    p.ecf = p.d.bg_ecf;
    p.w = ((ros_ld32(p.sp + SP_WIDTH) * 32) + (ros_ld32(p.sp + SP_RBIT) - ros_ld32(p.sp + SP_LBIT) + 1)) >> l2;
    if ((e = prepare_ttr(&p, &flags2, r7)) != NULL)
        goto out;
    if (p.in.kind != PF_PAL && p.out.kind == PF_PAL && p.ttr_type == TTR_NONE && reason == 56)
        goto out;                                   /* as for scaled plots: nothing */
    if (p.blending && p.out.kind == PF_PAL) {
        e = ros_error(0x1E6, "SpriteExtend: blending onto 256 colours or fewer needs BlendTable");
        goto out;
    }
    /* double pixels are not the transformed path's: BPC is BPP here */
    uint32_t sh = p.d.l2bpc - p.d.l2bpp;
    int32_t gwx0 = p.d.gwl << sh, gwx1 = (p.d.gwr + 1) << sh, gwy0 = p.d.gwb, gwy1 = p.d.gwt;
    p.d.l2bpc = p.d.l2bpp, p.d.bpc = p.d.bpp;

    /* the source rectangle */
    int32_t H = (int32_t)ros_ld32(p.sp + SP_HEIGHT) + 1;
    int32_t RE = (int32_t)((((ros_ld32(p.sp + SP_WIDTH) + 1) * 32) - (31 - ros_ld32(p.sp + SP_RBIT))) >> l2);
    int32_t lwp = (int32_t)(ros_ld32(p.sp + SP_LBIT) >> l2);
    int old = (p.mode >> 27) == 0;
    int32_t x0, y0, x1, y1;
    if (!(r3 & 2)) {
        x0 = old ? lwp : 0, y0 = H, x1 = RE, y1 = 0;
    } else {
        uint32_t b = s->r[4];
        x0 = (int32_t)ros_ld32(b), y0 = (int32_t)ros_ld32(b + 4);
        x1 = (int32_t)ros_ld32(b + 8), y1 = (int32_t)ros_ld32(b + 12);
        if (!(r3 & 1)) {
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
            y0 = CLAMP(y0, 0, H), y1 = CLAMP(y1, 0, H);
            if (x0 < 0) x0 = 0;
            if (x1 < 0) x1 = 0;
            x0 += lwp, x1 += lwp;
            if (x0 > RE) x0 = RE;
            if (x1 > RE) x1 = RE;
            if (x0 == x1 || y0 == y1) {
                e = ros_error(0x714, "SpriteExtend: Source rectangle area zero");
                goto out;
            }
        } else {
            if (x0 == x1 || y0 == y1) {
                e = ros_error(0x714, "SpriteExtend: Source rectangle area zero");
                goto out;
            }
            if (y0 < 0 || y1 < 0 || y0 > H || y1 > H || x0 < 0 || x1 < 0) {
                e = ros_error(0x713, "SpriteExtend: Source rectangle not inside sprite");
                goto out;
            }
            x0 += lwp, x1 += lwp;
            if (x0 > RE || x1 > RE) {
                e = ros_error(0x713, "SpriteExtend: Source rectangle not inside sprite");
                goto out;
            }
        }
    }

    /* the destination parallelogram, in 1/256 OS units */
    int32_t V[8];
    if (!(r3 & 1)) {
        int32_t m[6];
        for (int i = 0; i < 6; i++)
            m[i] = (int32_t)ros_ld32(s->r[6] + 4 * (uint32_t)i);
        int32_t ax0 = x0, ax1 = x1;
        if (old)
            ax0 -= lwp, ax1 -= lwp;
        int32_t X0 = ax0 << (p.m.xeig + 8), X1 = ax1 << (p.m.xeig + 8);
        int32_t Y0 = y0 << (p.m.yeig + 8), Y1 = y1 << (p.m.yeig + 8);
        V[0] = draw_x(m, X0, Y0, 0), V[1] = draw_x(m, X0, Y0, 1);
        V[2] = draw_x(m, X1, Y0, 0), V[3] = draw_x(m, X1, Y0, 1);
        V[6] = draw_x(m, X0, Y1, 0), V[7] = draw_x(m, X0, Y1, 1);
        V[4] = V[2] + V[6] - V[0], V[5] = V[3] + V[7] - V[1];
    } else {
        for (int i = 0; i < 8; i++)
            V[i] = (int32_t)ros_ld32(s->r[6] + 4 * (uint32_t)i);
        if (V[2] + V[6] - V[0] != V[4]) {               /* X only: Y's check is dead code */
            e = ros_error(0x715, "SpriteExtend can only do linear transformations");
            goto out;
        }
    }
    int32_t sx = p.d.xeig - (int32_t)sh, sy = p.d.yeig;       /* XEig + Log2BPP - Log2BPC */
    int32_t xv[4], yv[4];
    for (int k = 0; k < 4; k++) {
        xv[k] = (int32_t)((uint32_t)V[2 * k] + ((uint32_t)p.d.orgx << 8)) >> sx;
        yv[k] = (int32_t)((uint32_t)V[2 * k + 1] + ((uint32_t)p.d.orgy << 8)) >> sy;
    }

    /* the source frame */
    int32_t r8 = H - y0, r14 = H - y1;
    int32_t h = r8 > r14 ? r8 - r14 : r14 - r8, top = r8 < r14 ? r8 : r14;
    int32_t Ys0 = r8 - top, Ys3 = r14 - top;
    int32_t sleft = x0 < x1 ? x0 : x1, sright = x0 < x1 ? x1 : x0;
    int32_t Xs0 = x0, Wsz = x1 - x0, Hsz = Ys3 - Ys0;

    /* the inverse and the increments */
    int32_t bx1 = xv[1] - xv[0], by1 = yv[1] - yv[0], bx2 = xv[3] - xv[0], by2 = yv[3] - yv[0];
    int64_t det = (int64_t)bx1 * by2 - (int64_t)bx2 * by1, detp = det >> 2, inv = 0;
    if (detp) {
        uint64_t mag = detp < 0 ? (uint64_t)-detp : (uint64_t)detp;
        inv = (int64_t)((1ull << 62) / mag);
        if (detp < 0)
            inv = -inv;
    }
    uint32_t inc_Xx = produce_increment(by2, inv, Wsz), inc_Yx = produce_increment(-by1, inv, Hsz);
    uint32_t inc_Xy = produce_increment(-bx2, inv, Wsz), inc_Yy = produce_increment(bx1, inv, Hsz);

    /* rows from the top */
    int32_t ymax = yv[0];
    for (int k = 1; k < 4; k++)
        if (yv[k] > ymax)
            ymax = yv[k];
    int32_t R1 = (ymax - 128) >> 8;
    if (R1 > gwy1)
        R1 = gwy1;
    static const int edges[4][2] = { { 0, 1 }, { 1, 2 }, { 0, 3 }, { 3, 2 } };
    for (; R1 >= gwy0; R1--) {
        int64_t yc = (int64_t)R1 * 256 + 128;
        int32_t cx[2], n = 0;
        for (int k = 0; k < 4 && n < 2; k++) {
            int a = edges[k][0], b = edges[k][1];
            int lo = yv[a] <= yv[b] ? a : b, hi = lo == a ? b : a;
            if (yv[a] == yv[b])
                lo = a, hi = b;
            if (!((int64_t)yv[lo] < yc && yc <= (int64_t)yv[hi]))
                continue;
            cx[n++] = crossing(xv[lo], yv[lo], xv[hi], yv[hi], yc);
        }
        if (n < 2)
            break;
        int32_t lx = cx[0] < cx[1] ? cx[0] : cx[1], rx = cx[0] < cx[1] ? cx[1] : cx[0];
        if (lx < gwx0)
            lx = gwx0;
        if (rx > gwx1)
            rx = gwx1;
        if (lx >= rx)
            continue;
        int64_t dy = yc - yv[0];
        uint32_t Xr = ((uint32_t)Xs0 << 16) + (uint32_t)(int32_t)(((int64_t)dy * (int32_t)inc_Xy) >> 8);
        uint32_t Yr = ((uint32_t)Ys0 << 16) + (uint32_t)(int32_t)(((int64_t)dy * (int32_t)inc_Yy) >> 8);
        uint32_t value = 0;
        for (int32_t px = lx; px < rx; px++) {
            uint32_t k = (uint32_t)(px - (xv[0] >> 8));
            int32_t X = (int32_t)(k * inc_Xx + Xr), Y = (int32_t)(k * inc_Yx + Yr);
            int32_t yi = Y >> 16;
            if (yi < 0)
                yi = 0;
            if (yi >= h)
                yi = h - 1;
            int32_t xt = X;
            if (xt < (int32_t)((uint32_t)sleft << 16))
                xt = (int32_t)((uint32_t)sleft << 16);
            if (xt >= (int32_t)((uint32_t)sright << 16))
                xt = (int32_t)((uint32_t)sright << 16) - 1;
            uint32_t xi = (uint32_t)xt >> 16;
            plot_pixel(&p, px, R1, xi, (uint32_t)(top + yi), 0, &value);
        }
    }
out:
    free(p.wide);
    return e;
}
