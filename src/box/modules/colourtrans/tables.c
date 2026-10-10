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
 * (Sources/Video/Render/Colours: s.MainSWIs, s.Tables32K, s.TablesAlgo, MkTables/c.maketables).
 */

/* tables.c: ColourTrans's translation tables (MainSWIs, Tables32K,
 * TablesAlgo, and MkTables/c/maketables, which generates the ResourceFS
 * tables).
 *
 *   - SelectTable, SelectGCOLTable and GenerateTable make an entry for each
 *     colour of the source. The source can be a mode, a palette or a
 *     sprite's palette. Each entry is the nearest colour in the
 *     destination. An entry is a byte, or 2 or 4 bytes at 16 and 32 bpp
 *     when the new-style flag is set. Entries are GCOLs where that is asked
 *     for and possible. Alternatively the entries are the source's
 *     physical colours.
 *   - From 16 or 32 bpp to a palette, the result is a "32K" table. It has a
 *     byte for every 5-5-5 source colour, or 4-4-4 or 5-6-5. The table is
 *     built with the original's unweighted distance on truncated
 *     components. For the four palettes that ResourceFS holds tables for,
 *     the code uses those tables instead, made with maketables' weighted
 *     distance. Built tables last until the next mode change.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "ct.h"

#define GUARD_DOT 0x2E4B3233u               /* "32K." */
#define GUARD_PLUS 0x2B4B3233u              /* "32K+" */

/* ---- 32K tables --------------------------------------------------------------------------------- */

/* A built table. It holds the header, the palette the table was built for,
 * and the block. The header has the fields SrcNColour, SrcModeFlags,
 * SrcLog2BPP, DestCount and TableFormat. A HeaderSize of 20 bytes comes in
 * front of the table. */
struct built {
    struct built *next;
    uint32_t hdr[5], n, pal[256];
    uint32_t block;                     /* in the RMA: the 20-byte header, then the table */
};
static struct built *anchor;

void ct_free_tables(void)
{
    while (anchor) {
        struct built *b = anchor;
        anchor = b->next;
        ros_rma_free(ros_ptr(b->block));
        ros_rma_free(b);
    }
}

/* The four palettes that ResourceFS has tables for, and their tables. The
 * tables are made when first wanted and then kept. The original's are in
 * ROM. */
static const uint32_t desk4[16] = {
    0xFFFFFF00, 0xDDDDDD00, 0xBBBBBB00, 0x99999900, 0x77777700, 0x55555500, 0x33333300, 0x00000000,
    0x99440000, 0x00EEEE00, 0x00CC0000, 0x0000DD00, 0xBBEEEE00, 0x00885500, 0x00BBFF00, 0xFFBB0000,
};
static const uint32_t grey4[16] = {
    0xFFFFFF00, 0xDDDDDD00, 0xBBBBBB00, 0x99999900, 0x77777700, 0x55555500, 0x33333300, 0x00000000,
    0x11111100, 0xCCCCCC00, 0x66666600, 0x22222200, 0xEEEEEE00, 0x44444400, 0xAAAAAA00, 0x88888800,
};
static uint32_t rom_table[4];

static uint32_t resource_palette(int k, uint32_t i)
{
    switch (k) {
    case 0: return desk4[i];
    case 1: return grey4[i];
    case 2: {
        uint32_t v = (ct_modetwofivesix[i & 15] & 0x70307000u) | ct_hardbits[i >> 4];
        return v | v >> 4;
    }
    default: return i * 0x01010100u;
    }
}

/* maketables. The nearest entry is the one with the least 2r^2 + 4g^2 + b^2,
 * with the components scaled by 255/31. */
static uint32_t rom(int k)
{
    if (rom_table[k])
        return rom_table[k];
    uint32_t n = k < 2 ? 16 : 256, t = ros_addr(ros_rma_alloc(32768));
    for (uint32_t idx = 0; idx < 32768; idx++) {
        int32_t r = (int32_t)(idx & 31) * 255 / 31, g = (int32_t)((idx >> 5) & 31) * 255 / 31;
        int32_t b = (int32_t)(idx >> 10) * 255 / 31;
        uint32_t best = 0, bd = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p = resource_palette(k, i);
            int32_t dr = (int32_t)((p >> 8) & 255) - r, dg = (int32_t)((p >> 16) & 255) - g;
            int32_t db = (int32_t)(p >> 24) - b;
            uint32_t d = (uint32_t)(2 * dr * dr + 4 * dg * dg + db * db);
            if (d < bd)
                bd = d, best = i;
        }
        ros_st8(t + idx, best);
    }
    return rom_table[k] = t;
}

/* TablesAlgo. For each palette entry in turn, the entry's own cell is set
 * outright. Then every cell that the entry is strictly nearer to is set,
 * where the distance is |dr|<<e + |dg| + |db|<<e. */
static void build32k(uint8_t *out, const uint32_t *pal, uint32_t n, uint32_t rb, uint32_t gb,
                     uint32_t rgb_order)
{
    uint32_t entries = 1u << (2 * rb + gb), e = gb - rb;
    uint32_t rshift = 16 - rb, gshift = 24 - gb, bshift = 32 - rb;
    if (rgb_order) {
        uint32_t t = rshift;
        rshift = bshift, bshift = t;
    }
    static uint8_t err[65536];
    memset(err, 0xFF, entries);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t w = pal[k];
        int32_t pr = (int32_t)((w >> rshift) & ((1u << rb) - 1));
        int32_t pg = (int32_t)((w >> gshift) & ((1u << gb) - 1));
        int32_t pb = (int32_t)((w >> bshift) & ((1u << rb) - 1));
        uint32_t X = (uint32_t)pr | (uint32_t)pg << rb | (uint32_t)pb << (rb + gb);
        err[X] = 0, out[X] = (uint8_t)k;
        for (uint32_t idx = 0; idx < entries; idx++) {
            int32_t r = (int32_t)(idx & ((1u << rb) - 1)), g = (int32_t)((idx >> rb) & ((1u << gb) - 1));
            int32_t b = (int32_t)(idx >> (rb + gb));
            int32_t dr = r > pr ? r - pr : pr - r, dg = g > pg ? g - pg : pg - g;
            int32_t db = b > pb ? b - pb : pb - b;
            uint32_t d = ((uint32_t)dr << e) + (uint32_t)dg + ((uint32_t)db << e);
            if (d < err[idx])
                err[idx] = (uint8_t)d, out[idx] = (uint8_t)k;
        }
    }
}

/* make32Ktable. It writes 12 bytes at out: { guard, table, guard }. */
static os_error *make32k(uint32_t srcmode, uint32_t dmode, uint32_t dpal, uint32_t out)
{
    uint32_t dl2 = 0;
    ct_mode_var(dmode, 9, &dl2);
    /* The destination has 256 palette entries at most. The caller passes
     * only a destination below 16 bpp here. At 32 bpp, 1u << 32 would be
     * undefined in C, but the ObjAsm's MOV R11,R0,LSL R1 gives 0. */
    uint32_t n = dl2 >= 3 ? 256 : 1u << (1u << dl2), pal[256];
    memset(pal, 0, sizeof pal);
    os_error *e;
    if (dpal == 0xFFFFFFFFu) {
        uint32_t r[10] = { 0xFFFFFFFFu, 0xFFFFFFFFu, ct.scratch, n * 4, 0 };
        ct_swi(XColourTrans_ReadPalette, r, &e);
        if (e)
            return e;
        uint32_t got = (r[2] - ct.scratch) / 4;
        memcpy(pal, ros_ptr(ct.scratch), 4 * (got < n ? got : n));
        uint32_t q[10] = { 0 };
        ct_swi(XColourTrans_InvalidateCache, q, &e);
    } else if (dpal == 0) {
        /* The original writes only entries 0 to 63 for 8 bpp. The rest are
         * whatever the heap held, and are 0 here. Below 8 bpp the original
         * reads through an offset as if it were a pointer. Here the
         * default palette is used. */
        if (dl2 == 3) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t base = ct_modetwofivesix[i] | ct_modetwofivesix[i] >> 4;
                pal[i] = base | 0x88000000u | 0x00880000u;
                pal[16 + i] = pal[i] | 0x8800;
                pal[48 + i] = pal[16 + i] | 0x440000;
                pal[32 + i] = pal[48 + i] ^ 0x8800;
            }
        } else {
            uint32_t dn;
            const uint32_t *d = ct_defpal(dl2, &dn);
            memcpy(pal, d, 4 * (dn < n ? dn : n));
        }
    } else {
        for (uint32_t i = 0; i < n; i++)
            pal[i] = ros_ld32(dpal + 4 * i);
    }

    uint32_t nc = 0, f = 0, hdr[5], entries;
    ct_mode_var(srcmode, 3, &nc);
    if (nc > 65536) {
        entries = 32768, hdr[0] = 65535, hdr[1] = 0;
    } else {
        ct_mode_var(srcmode, 0, &f);
        hdr[1] = f & ~(uint32_t)MF_ALPHA, hdr[0] = nc;
        entries = (hdr[1] & 0x80) ? 65536 : nc + 1 == 4096 ? 4096 : 32768;
    }
    hdr[2] = 4, hdr[3] = n, hdr[4] = 0;
    int dot = hdr[0] + 1 == 65536 && hdr[1] == 0;
    uint32_t guard = dot ? GUARD_DOT : GUARD_PLUS, table = 0;

    for (struct built *b = anchor; b && !table; b = b->next)
        if (b->n == n && !memcmp(b->hdr, hdr, sizeof hdr) && !memcmp(b->pal, pal, 4 * n))
            table = b->block + 20;
    if (!table && dot)
        for (int k = 0; k < 4 && !table; k++) {
            uint32_t kn = k < 2 ? 16 : 256, i;
            if (kn != n)
                continue;
            for (i = 0; i < n && pal[i] == resource_palette(k, i); i++)
                ;
            if (i == n)
                table = rom(k);
        }
    if (!table) {
        struct built *b = ros_rma_alloc(sizeof *b);
        memset(b, 0, sizeof *b);
        memcpy(b->hdr, hdr, sizeof hdr), memcpy(b->pal, pal, sizeof pal), b->n = n;
        b->block = ros_addr(ros_rma_alloc(20 + entries));
        for (int i = 0; i < 5; i++)
            ros_st32(b->block + 4u * (uint32_t)i, i == 4 ? 20 : hdr[i]);
        uint32_t rb = entries == 4096 ? 4 : 5, gb = entries == 65536 ? 6 : rb;
        build32k(ros_ptr(b->block + 20), pal, n, rb, gb, hdr[1] & MF_RGB);
        b->next = anchor, anchor = b;
        ct.persist = 1;
        table = b->block + 20;
    }
    ros_st32(out, guard), ros_st32(out + 4, table), ros_st32(out + 8, guard);
    return NULL;
}

/* ---- SelectTable, SelectGCOLTable, GenerateTable ------------------------------------------------ */

static int is_area(uint32_t r0)
{
    if (r0 == 0xFFFFFFFFu || r0 < 256 || (r0 & 1))
        return 0;
    if (r0 == 0x8000 || r0 == 256)
        return 1;
    return !(ros_ld32(r0) & 1);
}

static uint32_t l2bpp(uint32_t mode)
{
    uint32_t v;
    return ct_mode_var(mode, 9, &v) ? 0 : v;
}

/* DecodeSprite's mode word, found by the route that ColourTrans_ReadPalette
 * itself takes. */
static os_error *sprite_mode(uint32_t area, uint32_t name, int ptr, uint32_t *mode)
{
    os_error *e;
    if (area == 0) {
        uint32_t r[10] = { 3 };
        ct_swi(XOS_ReadDynamicArea, r, &e);
        if (e)
            return e;
        area = r[0];
    }
    if (!ptr) {
        uint32_t r[10] = { 24 + 256, area, name };
        ct_swi(XOS_SpriteOp, r, &e);
        if (e)
            return e;
        name = r[2];
    }
    uint32_t r[10] = { 37 + 512, area, name, 0xFFFFFFFFu };
    ct_swi(XOS_SpriteOp, r, &e);
    if (e)
        return e;
    *mode = r[5];
    return NULL;
}

/* The flags come from the SWI. GenerateTable sets bit 31, which means the
 * flags are always checked. SelectTable passes R5 for a sprite area and 0
 * otherwise. SelectGCOLTable passes 8. */
os_error *ct_generate_table(struct ros_cpu *s, uint32_t flags)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3], r4 = s->r[4];
    int area = is_area(r0);
    flags &= ~0x80000000u;
    if (flags & ~0xFF00001Fu)
        return ct_err(CT_BADFLAGS);
    os_error *e;
    uint32_t src[256], nsrc = 0, size = 0;
    struct ct_pal p;
    int have = 0;

    if (area) {
        if (flags & 2) {
            uint32_t r[10] = { 37 + 256 + (flags & 1 ? 256 : 0), r0, r1, 0xFFFFFFFFu };
            ct_swi(XOS_SpriteOp, r, &e);
            if (e)
                return e;
            if (r[3] == 0) {                            /* no palette: use the mode's default */
                if ((e = ct_build(r[5], 0xFFFFFFFFu, &p)))
                    return e;
                memcpy(src, p.t, 4 * p.n), nsrc = p.n, have = 1;
            }
        }
        if (!have) {
            uint32_t m;
            if ((e = sprite_mode(r0, r1, flags & 1, &m)))
                return e;
            if (l2bpp(m) >= 4) {
                if (l2bpp(r3) >= 4)                     /* the palette is read as a mode */
                    goto finished;
            } else {
                uint32_t r[10] = { r0, r1, ct.scratch, 4096, flags & 1 };
                ct_swi(XColourTrans_ReadPalette, r, &e);
                if (e)
                    return e;
                nsrc = (r[2] - ct.scratch) / 4;
                memcpy(src, ros_ptr(ct.scratch), 4 * nsrc);
            }
        }
    } else {
        if ((e = ct_build(r0, r1, &p)))
            return e;
        memcpy(src, p.t, 4 * p.n), nsrc = p.n;
    }

    uint32_t srcmode = r0;
    if (area && (e = sprite_mode(r0, r1, flags & 1, &srcmode)))
        return e;
    uint32_t sl2 = l2bpp(srcmode), dl2 = l2bpp(r2), target = dl2;
    if (sl2 >= 4) {
        if (dl2 >= 4)
            size = 0;
        else if (r4 == 0)
            size = 12;
        else if ((e = make32k(srcmode, r2, r3, r4)))
            return e;
        goto finished;
    }
    uint32_t df = 0;
    if (ct_mode_var(r2, 0, &df))
        df = 0;
    if (df & MF_FULLPALETTE)
        flags &= ~8u;
    if (nsrc * 4 > 256)
        flags &= ~8u;
    if (dl2 != 3)
        flags &= ~8u;
    uint32_t dmode = r2, dpal = r3;
    if (dl2 > 3 && !(flags & 16))
        dmode = 21, dpal = 0, flags |= 8, target = 3;
    struct ct_pal d;
    if ((e = ct_build(dmode, dpal, &d)))
        return e;
    uint32_t width = ((1u << target) + 7) >> 3, fmt = flags >> 24;
    if (fmt >= 2)
        return ct_err(CT_BADFLAGS);
    for (uint32_t i = 0; i < nsrc; i++) {
        uint32_t w = src[i];
        if (flags & 4) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = w, c.r[12] = s->r[6];
            ros_call(&c, s->r[7]);
            w = c.r[0];
        }
        if (fmt == 0) {
            uint32_t v = ct_best(&d, w);
            if (flags & 8)
                v = ct_cn2g(v);
            for (uint32_t k = 0; k < width; k++, v >>= 8, size++)
                if (r4)
                    ros_st8(r4 + size, v & 255);
        } else {
            if (r4)
                ros_st32(r4 + size, w);
            size += 4;
        }
    }
finished:
    if (r4 == 0)
        s->r[4] = size;
    return NULL;
}
