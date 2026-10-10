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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts04, s.Fonts01).
 */

/* pixels.c: the character cache. It makes bitmaps from a master font's
 * outlines (s/Fonts04's CachePixels, cachebitmaps, convertchunk, drawchar,
 * convertcharpath, convertpath, the scaffold computations and stripoff).
 * It also holds the SWIs that make and forget them: Font_MakeBitmap and
 * Font_UnCacheFile (s/Fonts01).
 *
 * As in the original, a character is drawn through public calls. Its
 * outline becomes a Draw path in a scratch block. Output goes to a 1-bpp
 * sprite (OS_SpriteOp 60). Draw_Fill fills the path and Draw_Stroke draws
 * its skeleton lines. For an anti-aliased character the sprite is drawn
 * four times as big and Super_Sample makes it 4 bits per pixel. Then the
 * blank edges are cut off and the bits are packed, bottom row first, as a
 * bitmap font file holds them. Before any of that, the scaffold lines
 * (the hints) move the path's points to whole pixels, as the original
 * moves them.
 *
 * The output stays switched to the sprite until the SWI ends
 * (fm_restore_output), as the original leaves it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fm.h"

#define SCRATCHSIZE 12288               /* scratchsize: the Draw path's block */
#define SPR_DATA 60                     /* saExten + spPalette: the sprite's data in its area */
#define WID_LTANGENT 254
#define WID_RTANGENT 255
#define SK1_DONTBOTHER 1
#define SK4_DONTBOTHER 2
#define ERR_RESOURCEFS_READONLY 0x12EFCu   /* ErrorNumber_ResourceFS_FSReadOnly */

static int32_t sext12(uint32_t v) { return (int32_t)(v << 20) >> 20; }

/* Bits 16 to 47 of a * b. The original does SMULL, then lo >> 16 + hi << 16. */
static int32_t mul16(int32_t a, int32_t b)
{
    return (int32_t)(uint32_t)((uint64_t)((int64_t)a * b) >> 16);
}

/* ---- the state a conversion shares (the original's workspace) --------------------------------- */

static struct {
    struct fm_font *f, *m;              /* the slave, its master */
    const struct fm_mat *render;        /* rendermatrix: design units to pixels, scaled by 512 */
    int32_t pbox[4];                    /* pboxX, Y, W, H */
    int32_t xcomp, ycomp;               /* xcomponent, ycomponent: a composite part's offset */
    uint32_t matrix;                    /* the matrix for the Draw calls: 0, or the painting's */
    int32_t aspect;                     /* aspectratio: if >= 0, Super_Sample45 is used */
    uint8_t skel;                       /* skeleton_threshold */
    uint32_t scratch;                   /* scratchblock (RMA) */
    uint32_t spr_w, spr_h;              /* sprite_width (words), sprite_height (rows) */
} cx;

/* scaffoldoffsets, and the 15 words before it. The words before it keep
 * each line's original coordinate for linear links. Line i's offset is at
 * SCOFF(i) and its original is at SORIG(i). Line 0's original is its
 * offset. The array is never cleared. A link to a line that was not
 * computed for this character reads what an earlier character left. */
static uint32_t sw[32];
#define SCOFF(i) sw[16 + (i)]
#define SORIG(i) sw[16 - (i)]
static uint32_t sdone;                  /* if bit 16+i is clear, line i is still to be computed */

/* ---- the output sprite (outputtosprite, restoreoutput) ------------------------------------------ */

static uint32_t area_block, area_cap;   /* the RMA block whose end holds the sprite area */
static uint32_t sp_h, sp_addr, sp_w, sp_size;   /* sprite_params */
static uint32_t switchsave[4];          /* sprite_switchsave */
static int switched;

void fm_restore_output(void)
{
    if (!switched)
        return;
    uint32_t r[10] = { switchsave[0], switchsave[1], switchsave[2], switchsave[3] };
    os_error *e;
    switched = 0;
    sp_addr = 0;
    fm_swi(XOS_SpriteOp, r, &e);        /* (any error is ignored, so the caller's error is kept) */
}

/* outputtosprite: sets *none if the box is empty. Otherwise output goes to
 * a sprite big enough for it. The sprite is made afresh unless it is the
 * one already in use. */
static os_error *to_sprite(int *none)
{
    int bpp1 = fm_charflags & CHF_1BPP;
    int32_t W = cx.pbox[2], H = cx.pbox[3];
    uint32_t w, h;
    if (bpp1) {
        w = (uint32_t)(W + 31) >> 5;
        h = (uint32_t)H;
    } else {
        w = (uint32_t)(W + 10) >> 3;
        h = ((uint32_t)H * 4 + 3) | 3;
    }
    cx.spr_w = w, cx.spr_h = h;
    *none = W <= 0 || H <= 0;
    if (*none)
        return NULL;
    uint32_t size = h * w * 4 + SPR_DATA;
    uint32_t old = 0;
    if (size > area_cap) {
        old = area_block;
        area_cap = (size + 0xFFFF) & ~0xFFFFu;
        void *p = ros_rma_alloc(area_cap);
        if (!p) {
            area_cap = 0, area_block = 0;
            if (old)
                ros_rma_free(ros_ptr(old));
            return fm_err(FE_CACHEFULL, NULL, NULL);
        }
        area_block = ros_addr(p);
    }
    uint32_t a = area_block + area_cap - size;          /* at the end, so it tends not to move */
    os_error *e = NULL;
    if (h != sp_h || a != sp_addr || w != sp_w) {
        sp_h = h, sp_addr = a, sp_w = w, sp_size = size;
        ros_st32(a + 0, size);                          /* saEnd */
        ros_st32(a + 4, 1);                             /* saNumber */
        ros_st32(a + 8, 16);                            /* saFirst */
        ros_st32(a + 12, size);                         /* saFree */
        uint32_t sp = a + 16;
        ros_st32(sp + 0, size - 16);                    /* spNext */
        ros_st32(sp + 4, 'x'), ros_st32(sp + 8, 0), ros_st32(sp + 12, 0);
        ros_st32(sp + 16, w - 1);                       /* spWidth */
        ros_st32(sp + 20, h - 1);                       /* spHeight */
        ros_st32(sp + 24, 0);                           /* spLBit */
        ros_st32(sp + 28, 31);                          /* spRBit */
        ros_st32(sp + 32, 44), ros_st32(sp + 36, 44);   /* spImage, spTrans */
        ros_st32(sp + 40, 18);                          /* spMode: 1 bpp, 2 by 2 OS units to a pixel */
        uint32_t r[10] = { 60 + 512, a, sp, 0 };
        fm_swi(XOS_SpriteOp, r, &e);
        if (e)
            sp_addr = 0;
        else if (!switched) {
            memcpy(switchsave, r, sizeof switchsave);
            switched = 1;
        }
    }
    if (old)
        ros_rma_free(ros_ptr(old));
    return e;
}

/* ---- the scaffolding (convertcharpath, computescaffold...) --------------------------------------- */

static int32_t scaffold_x(int32_t c, uint32_t w, int32_t pre)   /* computescaffoldx */
{
    const struct fm_mat *m = cx.render;
    int32_t x = (int32_t)((uint32_t)c << m->cs) + cx.xcomp;
    if (!(fm_charflags & CHF_1BPP))
        x = (int32_t)((uint32_t)x << 2);
    int32_t mul, add;
    if (m->v[2] == 0)                   /* y contributes nothing, so use x's own term */
        mul = m->v[0], add = m->v[4];
    else
        mul = m->v[1], add = m->v[5];
    x = (int32_t)((uint32_t)add + (uint32_t)mul16(mul, x));
    x = (int32_t)((uint32_t)x + (uint32_t)pre);
    int32_t a, b;
    if (w == WID_LTANGENT || w == WID_RTANGENT) {
        int left = (w == WID_LTANGENT) == (mul >= 0);
        a = left ? 4 << 6 : -(4 << 6), b = left ? 1 << 6 : 7 << 6;
        return (int32_t)((((uint32_t)x + (uint32_t)a) & ~0x1FFu) + (uint32_t)b - (uint32_t)x);
    }
    int32_t lo = x >> 1;                /* in 1/256 pixel */
    int32_t h1 = mul16(mul, (int32_t)(w << m->cs));
    if (h1 < 0) {
        h1 = -h1;
        lo -= h1 >> 1;
    }
    int32_t q = h1 >> 2;
    int ge = q >= 64 / 2 + 1;
    q = -q & 0x7F;
    if (ge && q >= 192 / 2)
        q -= 256 / 2;
    uint32_t t = (((uint32_t)lo - (uint32_t)q + (1 << 7)) & ~0xFFu) + (uint32_t)q - (uint32_t)lo;
    return (int32_t)(t << 1);
}

static int32_t scaffold_y(int32_t c, uint32_t w, int32_t pre)   /* computescaffoldy */
{
    const struct fm_mat *m = cx.render;
    int32_t y = (int32_t)((uint32_t)c << m->cs) + cx.ycomp;
    if (!(fm_charflags & CHF_1BPP))
        y = (int32_t)((uint32_t)y << 2);
    int32_t mul, add;
    if (m->v[0] == 0)                   /* x contributes nothing, so use y's own term */
        mul = m->v[2], add = m->v[4];
    else
        mul = m->v[3], add = m->v[5];
    y = (int32_t)((uint32_t)add + (uint32_t)mul16(mul, y));
    y = (int32_t)((uint32_t)y + (uint32_t)pre);
    int32_t a, b;
    if (w == WID_LTANGENT || w == WID_RTANGENT) {
        int down = (w == WID_LTANGENT) == (mul >= 0);
        a = down ? 4 << 6 : -(4 << 6), b = down ? 1 << 6 : 7 << 6;
        return (int32_t)((((uint32_t)y + (uint32_t)a) & ~0x1FFu) + (uint32_t)b - (uint32_t)y);
    }
    int32_t lo = y >> 1;
    int32_t h1 = mul16(mul, (int32_t)(w << m->cs));
    if (h1 < 0) {
        h1 = -h1;
        lo -= h1 >> 1;
    }
    int32_t q = h1 >> 2;
    int ge = q >= 64 / 2 + 1;
    q = -q & 0x7F;
    if (ge && q >= 192 / 2)
        q -= 256 / 2;
    uint32_t t = (((uint32_t)lo - (uint32_t)q + (1 << 7)) & ~0xFFu) + (uint32_t)q - (uint32_t)lo;
    return (int32_t)(t << 1);
}

static int32_t compute(unsigned i);

/* computelink: the offset that line i takes from the line it is linked to.
 * For a linear link it takes the offset in proportion from the two lines
 * either side. `link` is bits 12 to 15 of the line's word. `raw` is line
 * i's word as defined. */
static int32_t compute_link(unsigned i, uint32_t link, uint32_t raw)
{
    unsigned j = (((i ^ (link >> 12)) & ~8u) ^ i) & 15;
    int32_t o1 = sdone & 1u << (16 + j) ? (int32_t)SCOFF(j) : compute(j);
    if (!(link & 0x8000))
        return o1;
    uint32_t a = SORIG(j);
    unsigned k = (((j ^ (a >> 12)) & ~8u) ^ j) & 15;
    int32_t o2 = (int32_t)SCOFF(k);
    uint32_t x1 = SORIG(j), x2 = SORIG(k), x = raw;
    if (x1 < (uint32_t)WID_LTANGENT << 16)             /* a thick line: use its centre */
        x1 += x1 >> 17;
    if (x2 < (uint32_t)WID_LTANGENT << 16)
        x2 += x2 >> 17;
    if (x < (uint32_t)WID_LTANGENT << 16)
        x += x >> 17;
    int32_t dx = sext12(x - x2), d = sext12(x1 - x2);
    if (d < 0)
        dx = -dx, d = -d;
    uint32_t prod = (uint32_t)(o1 - o2) * (uint32_t)dx;
    int neg = (int32_t)prod < 0;
    if (neg)
        prod = -prod;
    uint32_t q = d ? prod / (uint32_t)d : 0;            /* (division by zero gives 0, as UDIV does) */
    return neg ? (int32_t)((uint32_t)o2 - q) : (int32_t)((uint32_t)o2 + q);
}

/* computescaffold: line i's offset, from its definition */
static int32_t compute(unsigned i)
{
    sdone |= 1u << (16 + i);
    uint32_t raw = SCOFF(i);
    SORIG(i) = raw;
    uint32_t link = raw & 0xF000;
    int32_t pre = link ? compute_link(i, link, raw) : 0;
    uint32_t w = raw >> 16;
    int32_t c = sext12(raw);
    int32_t off = i < 8 ? scaffold_x(c, w, pre) : scaffold_y(c, w, pre);
    off = (int32_t)((uint32_t)off + (uint32_t)pre);
    SCOFF(i) = (uint32_t)off;
    return off;
}

/* the scaffold table's byte at off, or 0 beyond the end of the file */
static uint32_t sbyte(const uint8_t *tbl, const uint8_t *end, uint32_t off)
{
    return tbl + off < end ? tbl[off] : 0;
}

/* The first part of convertcharpath: the scaffold offsets for character
 * code. They come from its own lines and those of its base characters. */
static void scaffold(uint32_t code)
{
    for (int i = 0; i < 16; i++)
        SCOFF(i) = 0;
    if ((fm_switch_flags & SWF_ENABLED) && !(fm_switch_flags & SWF_SCAFFOLD))
        return;                         /* output to a buffer has no hints unless they are asked for */
    uint32_t wanted = 0xFFFFFFFFu;      /* if bit 16+i is set, line i is still wanted */
    const int32_t *v = cx.render->v;
    /* Drop the x lines if v[2] and v[3] are both non-zero. Drop the y
     * lines if v[0] and v[1] are both non-zero. */
    if (v[2] && v[3])
        wanted &= ~0x00FF0000u;
    if (v[0] && v[1])
        wanted &= ~0xFF000000u;
    struct fm_font *m = cx.m;
    struct fm_leaf *l = &m->leaf1;
    if (code >= l->nscaffolds || !l->data || m->scaffoldsize <= 512 || code == 0)
        return;
    const uint8_t *tbl = l->data + 52, *end = l->data + l->len;
    uint8_t flags = l->flags;
    sdone = 0xFFFFFFFFu;
    /* (a chain of base characters is a few long, but a file may make it a loop) */
    for (int depth = 0; depth < 64; depth++) {
        uint32_t at, base16, bits;
        if (!(flags & PP_BIGTABLE)) {
            uint32_t e = sbyte(tbl, end, 2 * code) | sbyte(tbl, end, 2 * code + 1) << 8;
            if (!e)
                break;
            base16 = flags & PP_16BITSCAFF ? 1 : e >> 15;
            if (!(flags & PP_16BITSCAFF))
                e &= ~0x8000u;
            at = e;
        } else {
            uint32_t e = sbyte(tbl, end, 4 * code) | sbyte(tbl, end, 4 * code + 1) << 8 |
                         sbyte(tbl, end, 4 * code + 2) << 16 | sbyte(tbl, end, 4 * code + 3) << 24;
            if (!e)
                break;
            base16 = e >> 31;
            at = e & 0x00FFFFFF;
            if (e & 0x40000000) {       /* no base character: bit 31 becomes line 0's bit */
                code = 0;
                bits = sbyte(tbl, end, at) << 16 | base16 << 16;
                goto local_y;
            }
        }
        code = sbyte(tbl, end, at);
        if (base16)
            code |= sbyte(tbl, end, ++at) << 8;
        bits = sbyte(tbl, end, ++at);                   /* the base character's x lines */
        bits |= sbyte(tbl, end, ++at) << 8;             /* the base character's y lines */
        bits |= sbyte(tbl, end, ++at) << 16;            /* local x lines */
    local_y:
        bits |= sbyte(tbl, end, ++at) << 24;            /* local y lines */
        for (unsigned i = 0; i < 16; i++) {
            uint32_t bit = 1u << (16 + i);
            if (!(bits & bit))
                continue;
            uint32_t lo = sbyte(tbl, end, ++at), hi = sbyte(tbl, end, ++at),
                     wd = sbyte(tbl, end, ++at);
            if (wanted & bit) {
                wanted &= ~bit;
                sdone &= ~bit;
                SCOFF(i) = lo | hi << 8 | wd << 16;
            }
        }
        wanted &= bits << 16;                           /* what the base defines, still wanted */
        if (!wanted || !code)
            break;
    }
    for (unsigned i = 0; i < 16; i++)
        if (!(sdone & 1u << (16 + i)))
            compute(i);
}

/* ---- paths (convertpath, convert_xy, getcoordpair) ----------------------------------------------- */

static void coord_pair(const uint8_t **pp, int32_t *x, int32_t *y)       /* getcoordpair */
{
    const uint8_t *p = *pp;
    if (fm_charflags & CHF_12BIT) {
        *x = (int32_t)((uint32_t)p[0] << 20 | (uint32_t)p[1] << 28) >> 20;
        *y = (int32_t)(p[1] >> 4) | (int32_t)((uint32_t)p[2] << 24) >> 20;
        *pp = p + 3;
    } else {
        *x = (int8_t)p[0], *y = (int8_t)p[1];
        *pp = p + 2;
    }
}

/* convert_xy: a point, transformed and hinted, stored at *out */
static void convert_xy(const uint8_t **pp, uint32_t sc, uint32_t *out)
{
    int32_t x, y;
    coord_pair(pp, &x, &y);
    x = (int32_t)((uint32_t)x + (uint32_t)cx.xcomp);
    y = (int32_t)((uint32_t)y + (uint32_t)cx.ycomp);
    fm_transform_pt(cx.render, &x, &y);
    uint32_t X = (uint32_t)x - ((uint32_t)cx.pbox[0] << 9), Y = (uint32_t)y - ((uint32_t)cx.pbox[1] << 9);
    if (!(fm_charflags & CHF_1BPP)) {
        X = (X << 2) + ((uint32_t)fm_antialiasx << 9) + (2 << 9);
        Y = (Y << 2) + ((uint32_t)fm_antialiasy << 9) + (2 << 9);
    }
    uint32_t sx = SCOFF((sc >> 2) & 7), sy = SCOFF(8 + ((sc >> 5) & 7));
    X += cx.render->v[0] ? sx : sy;
    Y += cx.render->v[3] ? sy : sx;
    ros_st32(*out, X), ros_st32(*out + 4, Y);
    *out += 8;
}

/* convertpath: the path at *pp, up to its terminator, as a Draw path in
 * the scratch block. *pp is left after the terminator. */
static os_error *convert_path(const uint8_t **pp)
{
    static const uint8_t codes[4] = { 0, 2, 8, 6 }, sizes[4] = { 4, 12, 12, 28 };
    uint32_t o = cx.scratch, lim = cx.scratch + SCRATCHSIZE - 8;
    const uint8_t *p = *pp;
    uint32_t old = 0;
    for (;;) {
        uint32_t b = *p++, t = b & 3;
        if (o + sizes[t] > lim) {
            *pp = p;
            return fm_err(FE_BUFFOVERFLOW, NULL, NULL);
        }
        ros_st32(o, codes[t]);
        o += 4;
        if (t == 0) {
            ros_st32(o, lim - o);                       /* the space left, after the end */
            break;
        }
        if (t == 3) {
            convert_xy(&p, old, &o);                    /* first control point: old hints */
            old = b;
            convert_xy(&p, old, &o);
        }
        old = b;
        convert_xy(&p, old, &o);
    }
    *pp = p;
    return NULL;
}

/* ---- characters (drawchar, drawcomposite, drawcomponent) ------------------------------------ */

/* getcharheader: sets the flags, keeping chf_1bpp, and returns the pointer past the box */
static const uint8_t *char_header(const uint8_t *p)
{
    uint8_t f = *p++;
    fm_charflags = (uint8_t)((f & ~CHF_1BPP) | (fm_charflags & CHF_1BPP));
    if ((f & CHF_OUTLINES) && (f & (CHF_COMPOSITE1 | CHF_COMPOSITE2)))
        return p;
    return p + (f & CHF_12BIT ? 6 : 4);
}

/* drawchar_tobuffer: copies the path in the scratch block onto the
 * buffer's. A closepath goes before each move except the first, and before
 * the end. The path is then transformed in the buffer by the painting's
 * matrix. The free space word after it is carried along. When only counting,
 * only the pointer moves. */
static os_error *to_buffer(void)
{
    int count = fm_switch_flags & SWF_JUSTCOUNT;
    uint32_t in = cx.scratch, out = fm_switch_buffer, start = out, free = count ? 0 : ros_ld32(out + 4);
    os_error *e = NULL;
#define PUT(v) do { if (count) out += 4; else if ((int32_t)(free -= 4) < 0) { free += 4; \
        e = fm_err(FE_BUFFOVERFLOW, NULL, NULL); } else { ros_st32(out, (v)); out += 4; } } while (0)
    uint32_t pending = 0;
    for (;;) {
        uint32_t w = ros_ld32(in);
        in += 4;
        if ((w == 2 || w == 0) && pending == 5)
            PUT(5);
        if (e || !w)
            break;
        PUT(w);
        pending = w == 2 ? 0 : 5;
        for (int n = w == 6 ? 6 : 2; n-- && !e; in += 4)
            PUT(ros_ld32(in));
        if (e)
            break;
    }
#undef PUT
    if (!count) {
        ros_st32(out, 0);
        ros_st32(out + 4, free);
        if (!e) {
            uint32_t r[10] = { start, 0, cx.matrix, 0 };
            fm_swi(XDraw_TransformPath, r, &e);
        }
    }
    if (!e)
        fm_switch_buffer = out;
    return e;
}

/* drawcomponent: fills the outline at *pp, then strokes its skeleton. If
 * output is switched to a buffer, it puts the outline into the buffer
 * instead. */
static os_error *draw_component(const uint8_t **pp, uint32_t code)
{
    if (fm_charflags & (CHF_COMPOSITE1 | CHF_COMPOSITE2))
        return NULL;
    scaffold(code);
    os_error *e = convert_path(pp);
    if (e)
        return e;
    if (fm_switch_buffer) {
        if ((e = to_buffer()) || !((*pp)[-1] & 4))
            return e;
        return convert_path(pp);                        /* (the skeleton is passed over) */
    }
    uint32_t r[10] = { cx.scratch, cx.m->leaf1.flags & PP_FILLNONZERO ? 0x30 : 0x32, cx.matrix, 100 };
    fm_swi(XDraw_Fill, r, &e);
    if (e || !((*pp)[-1] & 4))
        return e;
    e = convert_path(pp);
    if (fm_charflags & CHF_1BPP ? cx.skel & SK1_DONTBOTHER : cx.skel & SK4_DONTBOTHER)
        return e;
    if (!e) {
        uint32_t s[10] = { cx.scratch, 0x18, cx.matrix, 100, 0, 0, 0 };
        fm_swi(XDraw_Stroke, s, &e);
    }
    return e;
}

/* drawcomposite: the part code, at [xcomponent, ycomponent] */
static os_error *draw_part(uint32_t code)
{
    struct fm_chunkp ck;
    os_error *e = fm_load_chunk(cx.m, &cx.m->leaf1, code >> 5, &ck);
    if (e)
        return e;
    const uint8_t *p = fm_char_in(&ck, code);
    if (!p)
        return NULL;
    uint8_t keep = fm_charflags;
    p = char_header(p);
    e = draw_component(&p, code);
    fm_charflags = keep;
    return e;
}

static uint32_t read_code(const uint8_t **pp, uint8_t flags)
{
    uint32_t c = *(*pp)++;
    if (flags & CHF_16BITCODES)
        c |= (uint32_t)*(*pp)++ << 8;
    return c;
}

/* drawchar: p past the character's header */
static os_error *draw_char(const uint8_t *p, uint32_t code)
{
    cx.xcomp = cx.ycomp = 0;
    uint8_t flags = fm_charflags;
    os_error *e;
    if (flags & (CHF_COMPOSITE1 | CHF_COMPOSITE2)) {
        if (flags & CHF_COMPOSITE1 && (e = draw_part(read_code(&p, flags))))
            return e;
        if (!(fm_charflags & CHF_COMPOSITE2))
            return NULL;
        uint32_t c = read_code(&p, fm_charflags);
        coord_pair(&p, &cx.xcomp, &cx.ycomp);
        return draw_part(c);
    }
    if ((e = draw_component(&p, code)) || !(p[-1] & 8))
        return e;
    for (;;) {                          /* the parts that are included */
        uint32_t c = read_code(&p, flags);
        if (!c)
            return NULL;
        coord_pair(&p, &cx.xcomp, &cx.ycomp);
        if ((e = draw_part(c)))
            return e;
    }
}

/* ---- stripoff ------------------------------------------------------------------------------------ */

struct obuf { uint8_t *b; uint32_t n, cap; };

static void put8(struct obuf *o, uint8_t v)
{
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 4096;
        o->b = realloc(o->b, o->cap);
    }
    o->b[o->n++] = v;
}

static uint32_t or_column(uint32_t at, uint32_t len, uint32_t rows)
{
    uint32_t v = 0;
    for (uint32_t r = 0; r < rows; r++)
        v |= ros_ld32(at + r * len);
    return v;
}

static int row_blank(uint32_t at, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4)
        if (ros_ld32(at + i))
            return 0;
    return 1;
}

/* The image at `at`, made of rows of len bytes from the top, as a
 * character appended to o. It gives the header, then the bits from the
 * bottom row up, word aligned. The result is 0 if the image is blank. */
static int strip(uint32_t at, uint32_t len, uint32_t rows, struct obuf *o)
{
    int bpp1 = fm_charflags & CHF_1BPP;
    while (row_blank(at, len)) {
        if (!--rows)
            return 0;
        at += len;
    }
    uint32_t n = rows, left = 0, lcol = at, v;
    while (!(v = or_column(lcol, len, n)))
        lcol += 4, left += 32;
    uint32_t lv = v;
    if (!(v << 16)) left += 16, v >>= 16;
    if (!(v << 24)) left += 8, v >>= 8;
    if (!(v << 28)) left += 4, v >>= 4;
    if (bpp1) {
        if (!(v << 30)) left += 2, v >>= 2;
        if (!(v << 31)) left += 1;
    }
    uint32_t right = 0, rcol = at + len - 4;
    v = lv;
    if (rcol != lcol)
        while (!(v = or_column(rcol, len, n)))
            rcol -= 4, right += 32;
    if (!(v >> 16)) right += 16, v <<= 16;
    if (!(v >> 24)) right += 8, v <<= 8;
    if (!(v >> 28)) right += 4, v <<= 4;
    if (bpp1) {
        if (!(v >> 30)) right += 2, v <<= 2;
        if (!(v >> 31)) right += 1;
    }
    uint32_t bottom = at + (n - 1) * len, h = n;
    while (row_blank(bottom, len))
        h--, bottom -= len;
    int32_t x = cx.pbox[0] + (int32_t)(bpp1 ? left : left >> 2);
    int32_t y = cx.pbox[1] + (int32_t)(n - h);
    int32_t w = bpp1 ? (int32_t)(len * 8 - left - right) : (int32_t)(len * 2 - (left >> 2) - (right >> 2));
    int32_t hh = (int32_t)h;
    uint8_t flags = (uint8_t)(fm_charflags & CHF_1BPP);
    int fits = x == (int8_t)x && y == (int8_t)y && w == (int8_t)w && hh == (int8_t)hh;
    if (fits) {
        put8(o, flags), put8(o, (uint8_t)x), put8(o, (uint8_t)y), put8(o, (uint8_t)w), put8(o, (uint8_t)hh);
    } else {
        put8(o, flags | CHF_12BIT);
        put8(o, (uint8_t)x);
        int32_t yy = ((x >> 8) & 15) | (int32_t)((uint32_t)y << 4);
        put8(o, (uint8_t)yy), put8(o, (uint8_t)(yy >> 8));
        put8(o, (uint8_t)w);
        int32_t hv = ((w >> 8) & 15) | (int32_t)((uint32_t)hh << 4);
        put8(o, (uint8_t)hv), put8(o, (uint8_t)(hv >> 8));
    }
    /* the bits, least significant first: each row's useful ones, bottom row first */
    uint32_t nbits = len * 8 - right - left, acc = 0, nacc = 0;
    for (uint32_t r = 0; r < h; r++) {
        uint32_t row = bottom - r * len;
        for (uint32_t k = left; k < left + nbits; k++) {
            acc |= (ros_ld32(row + (k >> 5 << 2)) >> (k & 31) & 1) << nacc;
            if (++nacc == 8) {
                put8(o, (uint8_t)acc);
                acc = nacc = 0;
            }
        }
    }
    if (nacc)
        put8(o, (uint8_t)acc);
    while (o->n & 3)
        put8(o, 0);
    return 1;
}

/* ---- making a chunk (cachebitmaps, convertchunk) ---------------------------------------------------- */

/* One character. Its data is appended to o. *off is its offset from the
 * index, or 0 if it is blank or not in the outlines. */
static os_error *convert_char(const struct fm_chunkp *oc, uint32_t code, struct obuf *o, uint32_t *off)
{
    *off = 0;
    const uint8_t *p = oc->index ? fm_char_in(oc, code) : NULL;
    if (!p)
        return NULL;
    p = char_header(p);
    uint32_t data = sp_addr + SPR_DATA;
    for (uint32_t a = data; a < sp_addr + sp_size; a += 4)
        ros_st32(a, 0);
    os_error *e = draw_char(p, code);
    uint32_t len = cx.spr_w * 4, rows = cx.spr_h;
    if (!e && !(fm_charflags & CHF_1BPP)) {
        uint32_t r[10] = { 0, data, len, rows, data };
        fm_swi(cx.aspect >= 0 ? XSuper_Sample45 : XSuper_Sample90, r, &e);
        rows >>= 2;
    }
    if (e)
        return e;
    uint32_t at = o->n;
    if (strip(data, len, rows, o))
        *off = at;
    return NULL;
}

static void put_ix(uint8_t *ix, uint32_t v)
{
    ix[0] = (uint8_t)v, ix[1] = (uint8_t)(v >> 8), ix[2] = (uint8_t)(v >> 16), ix[3] = (uint8_t)(v >> 24);
}

static uint32_t get_ix(const uint8_t *ix) { return ix[0] | ix[1] << 8 | ix[2] << 16 | (uint32_t)ix[3] << 24; }

static void free_chunk(struct fm_chunk *k)
{
    if (k) {
        free(k->mem);
        free(k);
    }
}

/* cacheoutlines: finds the matrix, the master and its outline chunk c, and
 * allocates the scratch block */
static os_error *outlines_info(struct fm_font *f, uint32_t c, struct fm_chunkp *oc)
{
    os_error *e = NULL;
    if (!fm_trn && !f->render_ok && (e = fm_render_matrix(f)))
        return e;
    cx.f = f;
    cx.render = fm_trn ? &fm_trn->render : &f->render;
    cx.aspect = 2 * f->xres - 3 * f->yres;
    if ((e = fm_font_ptr(f->masterfont, &cx.m)) || (e = fm_load_chunk(cx.m, &cx.m->leaf1, c, oc)))
        return e;
    if (!cx.scratch) {
        void *p = ros_rma_alloc(SCRATCHSIZE);
        if (!p)
            return fm_err(FE_CACHEFULL, NULL, NULL);
        cx.scratch = ros_addr(p);
    }
    return NULL;
}

/* cachebitmaps: makes chunk c of the leaf l from the outlines. It makes
 * all of the chunk (code PIX_ALLCHARS) or one character into *slot. If
 * there is no chunk there, a split chunk is made for it. It does nothing if
 * the box is empty. */
static os_error *cache_bitmaps(struct fm_font *f, struct fm_leaf *l, uint32_t c, uint32_t code,
                               struct fm_chunk **slot)
{
    const int32_t *b = l->box;
    cx.pbox[0] = b[0], cx.pbox[1] = b[1], cx.pbox[2] = b[2] - b[0], cx.pbox[3] = b[3] - b[1];
    cx.matrix = 0;
    struct fm_chunkp oc;
    os_error *e = outlines_info(f, c, &oc);
    if (e)
        return e;
    int none;
    if ((e = to_sprite(&none)) || none)
        return e;
    uint32_t flags = (l->type & (PP_4XPOSNS | PP_4YPOSNS)) | PP_INCACHE;
    if (code != PIX_ALLCHARS)
        flags |= PP_SPLITCHUNK;
    unsigned nx = flags & PP_4XPOSNS ? 4 : 1, ny = flags & PP_4YPOSNS ? 4 : 1;
    uint32_t isize = 128 * nx * ny;
    uint8_t ax = fm_antialiasx, ay = fm_antialiasy;
    struct fm_chunk *k = code == PIX_ALLCHARS ? NULL : *slot;
    struct obuf o;
    if (k) {
        o = (struct obuf){ k->mem, k->len, k->len };
    } else {
        o = (struct obuf){ calloc(1, isize), isize, isize };
        if (code != PIX_ALLCHARS)
            for (uint32_t i = 0; i < isize; i += 4)
                put_ix(o.b + i, PIX_UNCACHED);
    }
    if (code != PIX_ALLCHARS) {
        /* the subpixel offsets that the chunk has, and 0 for the others */
        uint8_t x = flags & PP_4XPOSNS ? ax : 0, y = flags & PP_4YPOSNS ? ay : 0;
        unsigned block = x * ny + y;
        fm_antialiasx = x, fm_antialiasy = y;
        uint32_t off;
        e = convert_char(&oc, code, &o, &off);
        put_ix(o.b + 4 * (block * 32 + (code & 31)), e ? 0 : off);
        fm_antialiasx = ax, fm_antialiasy = ay;
        if (e && !k) {
            free(o.b);
            return e;
        }
        if (!k) {
            k = malloc(sizeof *k);
            k->flags = flags;
            *slot = k;
        }
        k->mem = o.b, k->len = o.n;
        return e;                       /* (the character is null if it failed) */
    }
    unsigned block = 0;
    for (unsigned x = 0; x < nx && !e; x++)
        for (unsigned y = 0; y < ny && !e; y++, block++) {
            fm_antialiasx = (uint8_t)x, fm_antialiasy = (uint8_t)y;
            for (uint32_t i = 0; i < 32 && !e; i++) {
                uint32_t off;
                e = convert_char(&oc, c * 32 + i, &o, &off);
                put_ix(o.b + 4 * (block * 32 + i), off);
            }
        }
    fm_antialiasx = ax, fm_antialiasy = ay;
    if (e) {
        free(o.b);
        return e;
    }
    k = malloc(sizeof *k);
    k->flags = flags, k->mem = o.b, k->len = o.n;
    *slot = k;
    return NULL;
}

void fm_delete_chunks(struct fm_leaf *l)
{
    for (uint32_t i = 0; i < l->nchunked; i++)
        free_chunk(l->chunks[i]);
    free(l->chunks);
    l->chunks = NULL;
    l->nchunked = 0;
}

/* The first part of setpixelsptr: the leaf's chunk array (readpixoblock) */
static os_error *chunk_array(struct fm_font *f, struct fm_leaf *l)
{
    cx.skel = f->skelthresh;
    if (l->type < LEAF_OUTLINES || l->type >= LEAF_FILE)
        return fm_err(FE_NOSUCHSWI, NULL, NULL);    /* (only outline leaves, and 4-bpp masters not yet) */
    struct fm_font *m;
    os_error *e = fm_font_ptr(f->masterfont, &m);
    if (e)
        return e;
    if (!l->chunks) {                   /* use the master's number of chunks */
        l->nchunks = m->leaf1.nchunks;
        l->nchunked = l->nchunks;
        l->chunks = calloc(l->nchunked ? l->nchunked : 1, sizeof *l->chunks);
    }
    return NULL;
}

os_error *fm_pixel_chunk(struct fm_font *f, struct fm_leaf *l, uint32_t c, struct fm_chunk **out)
{
    *out = NULL;
    os_error *e = chunk_array(f, l);
    if (e || c >= l->nchunked)
        return e;
    struct fm_chunk *k = l->chunks[c];
    if (k && (k->flags & PP_SPLITCHUNK)) {          /* made a character at a time, so make it again, whole */
        free_chunk(k);
        l->chunks[c] = k = NULL;
    }
    if (!k) {                                       /* CachePixels */
        fm_charflags = l == &f->leaf1 ? CHF_1BPP : 0;
        if ((e = cache_bitmaps(f, l, c, PIX_ALLCHARS, &l->chunks[c])))
            return e;
    }
    *out = l->chunks[c];
    return NULL;
}

/* getcharfromindex: the entry for code at the current subpixel offsets */
static uint32_t index_entry(const uint8_t *index, size_t avail, uint32_t flags, uint32_t code)
{
    uint32_t i = 0;
    if (flags & PP_4XPOSNS) {
        i = (uint32_t)fm_antialiasx << 5;
        if (flags & PP_4YPOSNS)
            i <<= 2;
    }
    if (flags & PP_4YPOSNS)
        i += (uint32_t)fm_antialiasy << 5;
    return 4 * ((size_t)i + (code & 31)) + 4 <= avail ? get_ix(index + 4 * (i + (code & 31))) : 0;
}

const uint8_t *fm_pixel_end;

os_error *fm_pixel_char(struct fm_font *f, struct fm_leaf *l, int bpp1, uint32_t g,
                        const uint8_t **out)
{
    *out = NULL;
    fm_pixel_end = NULL;
    uint32_t c = g >> 5;
    os_error *e;
    if (l->type >= LEAF_FILE && !fm_switch_buffer) {    /* the font has a bitmap file of its own */
        struct fm_chunkp ck;
        if ((e = fm_load_chunk(f, l, c, &ck)) || !ck.index)
            return e;
        size_t avail = (size_t)(ck.end - ck.index);
        uint32_t off = index_entry(ck.index, avail, ck.flags, g);
        *out = off && off < avail ? ck.index + off : NULL;
        fm_pixel_end = ck.end;
        return NULL;
    }
    if (l->type == LEAF_DIRECT || fm_switch_buffer) {   /* cache_fromoutlines_direct */
        fm_charflags = CHF_OUTLINES | CHF_1BPP;
        struct fm_chunkp oc;
        cx.skel = f->skelthresh;
        if ((e = outlines_info(f, c, &oc)))
            return e;
        *out = oc.index ? fm_char_in(&oc, g) : NULL;
        return NULL;
    }
    if ((e = chunk_array(f, l)) || c >= l->nchunked)
        return e;
    struct fm_chunk *k = l->chunks[c];
    uint32_t off = k ? index_entry(k->mem, k->len, k->flags, g) : PIX_UNCACHED;
    if (off == PIX_UNCACHED) {                      /* CachePixels, for this character */
        fm_charflags = bpp1 ? CHF_1BPP : 0;
        e = cache_bitmaps(f, l, c, g, &l->chunks[c]);
        k = l->chunks[c];
        if (e || !k)
            return e;
        off = index_entry(k->mem, k->len, k->flags, g);
    }
    *out = off && off != PIX_UNCACHED && off < k->len ? k->mem + off : NULL;
    fm_pixel_end = k->mem + k->len;
    return NULL;
}

void fm_draw_outline(struct fm_font *f, const uint8_t *p, uint32_t code, uint32_t matrix, os_error **e)
{
    struct fm_chunkp oc;
    cx.pbox[0] = cx.pbox[1] = 0;
    cx.skel = f->skelthresh;
    if ((*e = outlines_info(f, code >> 5, &oc)))
        return;
    cx.matrix = matrix;
    *e = draw_char(p, code);
    cx.matrix = 0;
}

/* ---- Font_UnCacheFile ---------------------------------------------------------------------------- */

static int same_name(const char *a, const char *b)                 /* matchname */
{
    while (*a == ' ')
        a++;
    for (;; a++, b++) {
        int x = (uint8_t)*a, y = (uint8_t)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/* deletechunks_R7 for a leaf */
static void delete_leaf(struct fm_font *f, struct fm_leaf *l)
{
    fm_delete_chunks(l);
    if (l->type >= LEAF_DIRECT && l->type < LEAF_FILE)
        f->render_ok = 0;               /* the derived data has gone, so the render matrix goes too */
    free(l->data);                      /* tidyfiles. An Outlines file's scaffolding goes with it. */
    l->data = NULL;
    l->type = LEAF_SCAN;
}

/* tryuncache */
static void try_uncache(struct fm_font *f, struct fm_leaf *l, const char *name, int recache)
{
    char met[12], out[12], full[320];
    if (fm_leafnames(f, met, out))
        return;
    const char *leaf;
    if (!l)
        leaf = met;
    else if (l->type <= LEAF_SCAN)
        return;
    else
        leaf = l->type < LEAF_FILE ? out : l->name;
    if (fm_file_name(f, leaf, full, sizeof full) || !same_name(full, name))
        return;
    if (l) {
        delete_leaf(f, l);
        return;
    }
    fm_forget_metrics(f);
    if (recache == 1 && f->masterflag == MSF_MASTER)
        fm_metrics_header(f);
}

static void uncache(const char *name, int recache)
{
    for (int pass = 0; pass < 2; pass++)            /* slaves first, then masters */
        for (uint32_t h = 1; h < 256; h++) {
            struct fm_font *f = fm_fonts[h];
            if (!f || (f->masterflag == MSF_MASTER) != pass)
                continue;
            try_uncache(f, NULL, name, recache);
            try_uncache(f, &f->leaf1, name, recache);
            try_uncache(f, &f->leaf4, name, recache);
            char met[12], out[12], full[320];
            if (f->masterflag != MSF_MASTER && f->trns && !fm_leafnames(f, met, out) &&
                !fm_file_name(f, out, full, sizeof full) && same_name(full, name))
                fm_forget_transforms(f);                /* uncachetransforms */
        }
}

void fm_thunk_UnCacheFile(struct ros_cpu *s)
{
    char name[256];
    uint32_t a = s->r[1];
    size_t i = 0;
    while (ros_ld8(a) == ' ')
        a++;
    for (; i < sizeof name - 1; i++) {
        uint8_t c = (uint8_t)ros_ld8(a + i);
        if (c <= ' ')
            break;
        name[i] = (char)c;
    }
    name[i] = 0;
    uncache(name, (int)s->r[2]);
    s->v = 0;
}

/* ---- Font_MakeBitmap -------------------------------------------------------------------------------- */

/* GetNewPixelsHeader_R7: decides again where the leaf's data comes from,
 * unless it is undecided or from a file. If that changes, what was made
 * from the old data is deleted. */
static os_error *new_pixels_header(struct fm_font *f, struct fm_leaf *l)
{
    uint8_t old = l->type;
    if (old == LEAF_SCAN || old >= LEAF_FILE)
        return NULL;
    l->type = LEAF_SCAN;
    os_error *e = fm_pixels_header(f, l);
    if (l->type != old)
        fm_delete_chunks(l);
    return e;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static os_error *gbpb(uint32_t op, uint32_t h, const void *data, uint32_t n, uint32_t at)
{
    uint8_t *buf = ros_rma_alloc(n ? n : 4);
    if (!buf)
        return fm_err(FE_NOROOM, NULL, NULL);
    memcpy(buf, data, n);
    uint32_t r[10] = { op, h, ros_addr(buf), n, at };
    os_error *e;
    fm_swi(XOS_GBPB, r, &e);
    ros_rma_free(buf);
    if (!e && r[3])
        e = fm_err(FE_BADFONTFILE, NULL, NULL);
    return e;
}

/* OS_File or OS_Find on a name */
static os_error *file_op(uint32_t swi, uint32_t op, const char *name, uint32_t r2, uint32_t *r0)
{
    uint32_t a = ros_addr(ros_rma_alloc((uint32_t)strlen(name) + 1));
    strcpy(ros_ptr(a), name);
    uint32_t r[10] = { op, a, r2 };
    os_error *e;
    fm_swi(swi, r, &e);
    ros_rma_free(ros_ptr(a));
    if (r0)
        *r0 = r[0];
    return e;
}

/* The header, on the stack, up to its first pixoffset. *len is its length. */
static os_error *bitmap_header(struct fm_font *f, struct fm_leaf *l, uint32_t flags, uint8_t *h,
                               uint32_t *len)
{
    memset(h, 0, 256);
    memcpy(h, "FONT", 4);
    h[4] = flags & 1 ? 4 : 1;
    h[5] = 8;
    h[6] = (uint8_t)(PP_FLAGSINFILE | (flags & 6) >> 1);
    put32(h + 8, ((uint32_t)l->box[0] & 0xFFFF) | (uint32_t)l->box[1] << 16);
    put32(h + 12, (uint32_t)(l->box[2] - l->box[0]) | (uint32_t)(l->box[3] - l->box[1]) << 16);
    put32(h + 52, 10 | (uint32_t)f->xsize << 16);
    put32(h + 56, (uint32_t)f->xres | (uint32_t)f->ysize << 16);
    put32(h + 60, (uint32_t)f->yres);
    uint32_t p = 62;
    for (int i = 0; i < 40 && (uint8_t)f->name[i] >= 32; i++)
        h[p++] = (uint8_t)f->name[i];
    h[p++] = 0;
    /* four OS_ConvertCardinal4 conversions, sharing 16 bytes of room */
    uint32_t nums[4] = { (uint32_t)f->xsize >> 4, (uint32_t)f->ysize >> 4, (uint32_t)f->xres,
                         (uint32_t)f->yres };
    static const char *const after[4] = { "x", " points at ", "x", " dpi" };
    uint32_t room = 16;
    for (int i = 0; i < 4; i++) {
        char d[16];
        uint32_t n = (uint32_t)snprintf(d, sizeof d, "%u", nums[i]);
        if (n + 1 > room)
            return fm_err(FE_BUFFOVERFLOW, NULL, NULL);
        room -= n;
        memcpy(h + p, d, n), p += n;
        size_t k = strlen(after[i]);
        memcpy(h + p, after[i], k), p += (uint32_t)k;
    }
    h[p] = 0;
    *len = (p + 4) & ~3u;
    return NULL;
}

static os_error *make_bitmap(struct ros_cpu *s, uint32_t *lose)
{
    uint32_t flags = s->r[6];
    uint32_t bad = ~15u;
    if (!(flags & 1))
        bad |= 6;
    if (flags & bad)
        return fm_err(FE_RESERVED, NULL, NULL);
    uint32_t h;
    os_error *e;
    if (s->r[1] < 256) {
        h = s->r[1];
    } else {
        uint32_t r[10] = { 0, s->r[1], s->r[2], s->r[3], s->r[4], s->r[5] };
        fm_swi(XFont_FindFont, r, &e);
        if (e)
            return e;
        h = r[0];
        *lose = 1;
    }
    struct fm_font *f;
    if ((e = fm_font_ptr(h, &f)))
        return e;
    fm_set_paint_matrix(NULL);
    fm_trn = NULL;
    int bpp4 = flags & 1;
    struct fm_leaf *l = bpp4 ? &f->leaf4 : &f->leaf1;
    int32_t xs = f->xscale * f->xmag, ys = f->yscale * f->ymag;
    char leaf[12], name[320];
    char big[32];
    if (snprintf(big, sizeof big, "%c%ux%u", bpp4 ? 'f' : 'b', (unsigned)xs / 72, (unsigned)ys / 72) > 10)
        return fm_err(FE_TOOLONG, NULL, NULL);          /* (convertfilename's limit) */
    memcpy(leaf, big, sizeof leaf);
    if ((e = fm_file_name(f, leaf, name, sizeof name)))
        return e;
    uncache(name, 0);
    if ((e = file_op(XOS_File, 6, name, 0, NULL)))
        return e;
    if (flags & 8)
        return NULL;
    /* make it from outlines, cached, anti-aliased, with subpixels as asked */
    int32_t saved[5];
    memcpy(saved, f->threshold, sizeof saved);
    f->threshold[0] = 0;
    f->threshold[1] = f->threshold[2] = 0x20000000;
    f->threshold[3] = flags & 2 ? 0x20000000 : 0;
    f->threshold[4] = flags & 4 ? 0x20000000 : 0;
    e = new_pixels_header(f, l);
    if (!e)
        e = fm_pixels_header(f, l);
    uint8_t hdr[256];
    uint32_t hlen = 0, fh = 0;
    struct fm_chunk *k;
    if (!e)
        e = bitmap_header(f, l, flags, hdr, &hlen);
    if (!e)
        e = fm_pixel_chunk(f, l, 0, &k);                /* chunk 0, to learn the number of chunks */
    if (!e) {
        uint32_t nchunks = l->nchunks;
        put32(hdr + 16, hlen);
        put32(hdr + 20, nchunks);
        put32(hdr + 24, 1);                             /* a bogus scaffold table of one entry */
        put32(hdr + hlen, hlen + (nchunks + 1) * 4);
        e = file_op(XOS_Find, 0x83, name, 0, &fh);                /* OpenOut, no path */
        if (!e) {
            e = file_op(XOS_File, 18, name, 0xFF6, NULL);
            if (!e)
                e = gbpb(2, fh, hdr, hlen + 4, 0);
            uint32_t at = hlen + (nchunks + 1) * 4;
            for (uint32_t c = 0; !e && c < l->nchunks; c++) {
                if ((e = fm_pixel_chunk(f, l, c, &k)))
                    break;
                if (k) {
                    uint8_t *mem = malloc(4 + k->len);
                    put32(mem, (k->flags & 3) | PP_FLAGSPRESENT);
                    memcpy(mem + 4, k->mem, k->len);
                    e = gbpb(1, fh, mem, 4 + k->len, at);
                    free(mem);
                    at += 4 + k->len;
                }
                uint8_t w[4];
                put32(w, at);
                if (!e)
                    e = gbpb(1, fh, w, 4, hlen + 4 * (c + 1));
            }
            uint32_t r[10] = { 0, fh };
            os_error *e2;
            fm_swi(XOS_Find, r, &e2);
            if (!e)
                e = e2;
        }
    }
    /* Put the thresholds back into the current font's header. This may not be
     * the font used above, if that font was given by its handle. */
    struct fm_font *cur;
    if (!fm_font_ptr(fm_currentfont, &cur))
        memcpy(cur->threshold, saved, sizeof saved);
    return e;
}

void fm_thunk_MakeBitmap(struct ros_cpu *s)
{
    uint32_t lose = 0;
    os_error *e = make_bitmap(s, &lose);
    if (lose)
        fm_lose_font(fm_currentfont);
    fm_restore_output();
    if (e && e->errnum == ERR_RESOURCEFS_READONLY)
        e = fm_err(FE_NOBITMAPS, NULL, NULL);
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}
