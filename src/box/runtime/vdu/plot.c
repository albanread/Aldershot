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
 * This file is a reimplementation in C, in places transliterated, of RISC OS
 * Open's Kernel source (Sources/Kernel: s.vdu.vduplot, s.vdu.vdugrafa,
 * s.vdu.vdugrafd, s.vdu.vdugrafe, s.vdu.vdugraff, s.vdu.vdudriver).
 */
/* plot.c -- PLOT: points, lines, fills, polygons, block copy, and the
 * colours, GCOL actions and ECF patterns they are plotted with.
 *
 * This follows the kernel's Kernel/s/vdu/vduplot, vdugrafa, vdugrafd,
 * vdugrafe, vdugraff and vdudriver (SetColour). It is transliterated
 * where the pixels depend on the order of the arithmetic.
 *
 *   - Co-ordinates. An external co-ordinate (x + OrgX) ASR XEig is
 *     internal, and pixels are counted from the bottom left. GCsX/Y is set
 *     before the shape is drawn, and the internal cursors shuffle after it
 *     (CTidy).
 *   - Every pixel is written as (screen OR ora) EOR eor. The ora and eor
 *     come from an eight-row table chosen by the plot code's low bits:
 *     nothing, the foreground, invert or the background. The table is
 *     indexed by the screen row mod 8. The tables are built from the GCOL
 *     colour or ECF pattern, the action (TBscrmasks), transparency and the
 *     ECF origin (SetColour).
 *   - Lines are the kernel's Bresenham from the previous point to this one.
 *     They are clipped pixel by pixel, and clipping never changes which
 *     pixels are lit. Dotted lines step the VDU 23,6 pattern for every
 *     pixel, clipped or not.
 *   - Triangles and parallelograms draw each row once, from the edges'
 *     Bresenham pixels. The vertices are sorted by y and then by descending
 *     x. Rectangles are inclusive.
 *   - Line fills and flood fills compare whole pixels against the
 *     unrotated fg/bg pattern. The flood fill is the kernel's FIFO of
 *     spans, 16383 deep, and it stops silently when the FIFO is full.
 *   - Block copy and move make a pixel copy of what is in the window. The
 *     rest of the destination is cleared to the background with the store
 *     action, and so is the uncovered source when moving.
 *
 * Circles, arcs, segments, sectors and ellipses are in shapes.c. VDU 5
 * characters are in vdu5.c.
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "vduws.h"
#include "plot.h"

#define UKPLOTV 0x19u

static const uint32_t no_effect[16];
static const uint32_t invert_tbl[16] = {
    0, ~0u, 0, ~0u, 0, ~0u, 0, ~0u, 0, ~0u, 0, ~0u, 0, ~0u, 0, ~0u,
};

const uint32_t *plot_gcol;              /* GColAdr */

/* ---- the geometry of a pixel ------------------------------------------------- */

static uint32_t xshift(void)
{
    return 5 - vdu.mv[MV_LOG2BPC];
}

uint32_t plot_npix(void)
{
    return (32u >> vdu.mv[MV_LOG2BPC]) - 1;
}

uint32_t *plot_word(int32_t x, int32_t y)
{
    uint32_t row = (uint32_t)((int32_t)vdu.mv[MV_YWIND] - y);
    return (uint32_t *)(vdu.screen + row * vdu.mv[MV_LINELENGTH]) + ((uint32_t)x >> xshift());
}

uint32_t plot_pmask(int32_t x)
{
    if (vdu.bpc == 32)
        return ~0u;
    return ((1u << vdu.bpc) - 1) << (((uint32_t)x & plot_npix()) * vdu.bpc);
}

uint32_t plot_erow(int32_t y)
{
    return (uint32_t)((int32_t)vdu.mv[MV_YWIND] - y) & 7;
}

int plot_in_window(int32_t x, int32_t y)
{
    return vdu.gwl <= x && x <= vdu.gwr && vdu.gwb <= y && y <= vdu.gwt;
}

void plot_write(int32_t x, int32_t y)
{
    const uint32_t *t = plot_gcol + 2 * plot_erow(y);
    uint32_t *w = plot_word(x, y), m = plot_pmask(x);
    *w = (*w | (t[0] & m)) ^ (t[1] & m);
}

/* PlotPoint */
void plot_point(int32_t x, int32_t y)
{
    if (plot_in_window(x, y))
        plot_write(x, y);
}

/* NewHLine: xl <= xr, both inclusive */
void plot_new_hline(int32_t xl, int32_t y, int32_t xr)
{
    if (!(vdu.gwt >= y && y >= vdu.gwb && vdu.gwr >= xl && xr >= vdu.gwl))
        return;
    if (xl < vdu.gwl)
        xl = vdu.gwl;
    if (xr > vdu.gwr)
        xr = vdu.gwr;
    const uint32_t *t = plot_gcol + 2 * plot_erow(y);
    uint32_t ora = t[0], eor = t[1], xs = xshift(), l2 = vdu.mv[MV_LOG2BPC];
    uint32_t *p = plot_word(xl, y);
    uint32_t n = ((uint32_t)xr >> xs) - ((uint32_t)xl >> xs);
    uint32_t b0 = ((uint32_t)xl << l2) & 31, b1 = (((uint32_t)xr << l2) & 31) + (1u << l2);
    uint32_t lm = ~0u << b0, rm = b1 >= 32 ? ~0u : ~(~0u << b1);
    if (n == 0) {
        uint32_t m = lm & rm;
        *p = (*p | (ora & m)) ^ (eor & m);
        return;
    }
    *p = (*p | (ora & lm)) ^ (eor & lm);
    p++;
    for (uint32_t i = 1; i < n; i++, p++)
        *p = (*p | ora) ^ eor;
    *p = (*p | (ora & rm)) ^ (eor & rm);
}

/* HLine: the ends in either order */
void plot_hline(int32_t a, int32_t y, int32_t b)
{
    if (a > b) {
        int32_t t = a;
        a = b, b = t;
    }
    plot_new_hline(a, y, b);
}

/* RectFillA: any two opposite corners, rows top to bottom */
void plot_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 > x1) {
        int32_t t = x0;
        x0 = x1, x1 = t;
    }
    if (y1 > y0) {
        int32_t t = y0;
        y0 = y1, y1 = t;
    }
    for (int32_t y = y0; y >= y1; y--)
        plot_new_hline(x0, y, x1);
}

/* A pixel's value, and a pixel stored raw (block copy) */
static uint32_t get_pixel(int32_t x, int32_t y)
{
    uint32_t m = plot_pmask(x);
    return *plot_word(x, y) & m;
}

static void put_pixel(int32_t x, int32_t y, uint32_t v)
{
    uint32_t m = plot_pmask(x), *w = plot_word(x, y);
    *w = (*w & ~m) | (v & m);
}

/* ---- colours: SetColour --------------------------------------------------------- */

static uint32_t ror32(uint32_t v, uint32_t n)
{
    n &= 31;
    return n ? v >> n | v << (32 - n) : v;
}

static uint32_t add_tint(uint32_t c, uint32_t tint)
{
    return vdu_gcol_to_colour((c & 63) | (tint & 0xC0));
}

static uint32_t le16(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8;
}

static uint32_t le32(const uint8_t *b)
{
    return le16(b) | le16(b + 2) << 16;
}

/* SetCol10. This makes the eight-row pattern of a colour, ECF or
 * OS_SetColour pattern. */
static void setcol10(uint32_t colour, uint32_t action, uint32_t dst[8], uint32_t tint,
                     const uint32_t pat[8])
{
    uint32_t sel = action & 0xF0, nc = vdu.mv[MV_NCOLOUR], bpp = vdu.bpp;
    if (sel == 0) {
        uint32_t c = colour & nc & 63;
        if (nc & 0xF0)
            c = add_tint(c, tint);
        for (uint32_t s = bpp; s < 32; s <<= 1)
            c |= c << s;
        for (int i = 0; i < 8; i++)
            dst[i] = c;
        return;
    }
    if (sel >= 96) {
        memcpy(dst, pat, 32);
        return;
    }
    if (sel >= 80) {                    /* the giant ECF: all four side by side */
        for (int r = 0; r < 8; r++)
            dst[r] = vdu.ecf[0][r] | (uint32_t)vdu.ecf[1][r] << 8 |
                     (uint32_t)vdu.ecf[2][r] << 16 | (uint32_t)vdu.ecf[3][r] << 24;
        return;
    }
    const uint8_t *e = vdu.ecf[(sel >> 4) - 1];
    if (bpp > 16) {
        for (int r = 0; r < 8; r++)
            dst[r] = le32(e + (r & 1) * 4);
    } else if (bpp == 16) {
        for (int r = 0; r < 8; r++) {
            uint32_t h = le16(e + 2 * (r & 3));
            dst[r] = h | h << 16;
        }
    } else if (bpp != vdu.bpc) {        /* double-pixel modes: each pixel twice */
        uint32_t pm = nc & 0xF0 ? 0xFF : nc;
        for (int r = 7; r >= 0; r--) {
            uint32_t b = e[r], out = 0;
            int ex = 8 - (int)bpp, ins = 32 - (int)bpp;
            do {
                uint32_t pix = pm & ror32(b, (uint32_t)ex);
                out |= pix << ins;
                ins -= (int)bpp;
                out |= pix << ins;
                ex = (ex - (int)bpp) & 7;
                ins -= (int)bpp;
            } while (ins >= 0);
            dst[r] = out;
        }
    } else {
        for (int r = 0; r < 8; r++)
            dst[r] = e[r] * 0x01010101u;
    }
}

/* SetCol60. This turns a pattern and an action into eight {ora, eor}
 * rows. */
static void setcol60(const uint32_t src[8], const uint32_t other[8], uint32_t *dst,
                     uint32_t action)
{
    action &= 0xF;
    uint32_t nib = ror32(0x970FEC53u, 4 * (action & 7)) >> 28;    /* TBscrmasks */
    int n = nib >> 3 & 1, z = nib >> 2 & 1, c = nib >> 1 & 1, v = nib & 1;
    uint32_t pm0 = vdu.bpc == 32 ? ~0u : (1u << vdu.bpc) - 1;
    for (int r = 7; r >= 0; r--) {
        uint32_t col = src[r], m = ~0u;
        if (action & 8) {               /* transparent where it equals the other */
            uint32_t diff = other[r] ^ col;
            m = 0;
            for (uint32_t pm = pm0; pm; pm = vdu.bpc == 32 ? 0 : pm << vdu.bpc)
                if (diff & pm)
                    m |= pm;
        }
        uint32_t eor = c ? ~0u : col;
        if (v)
            eor = ~eor;
        uint32_t ora = n ? ~0u : col;
        if (z)
            ora = ~ora;
        ora &= m, eor &= m;
        uint32_t row = (vdu.ecf_yoffset + (uint32_t)r) & 7;
        dst[2 * row] = ror32(ora, vdu.ecf_shift);
        dst[2 * row + 1] = ror32(eor, vdu.ecf_shift);
    }
}

void vdu_set_colour(void)
{
    if (!vdu.fg_oe)
        return;
    setcol10(vdu.gfcol, vdu.gplfmd, vdu.fg_ecf, vdu.gftint, vdu.fg_pattern);
    setcol10(vdu.gbcol, vdu.gplbmd, vdu.bg_ecf, vdu.gbtint, vdu.bg_pattern);
    setcol60(vdu.fg_ecf, vdu.bg_ecf, vdu.fg_oe, vdu.gplfmd);
    setcol60(vdu.bg_ecf, vdu.fg_ecf, vdu.bg_oe, vdu.gplbmd);
    setcol60(vdu.bg_ecf, vdu.fg_ecf, vdu.bg_store, 0);
}

void vdu_plot_init(void)
{
    uint32_t *t = ros_rma_alloc(3 * 64);
    memset(t, 0, 3 * 64);
    vdu.fg_oe = t, vdu.bg_oe = t + 16, vdu.bg_store = t + 32;
    vdu.oe_tables = ros_addr(t);
}

/* GetECFIndex */
static uint32_t ecf_index(void)
{
    static const uint8_t idx[6] = { 4, 2, 3, 5, 5, 5 };
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC)
        return 0;
    if (vdu.mv[MV_LOG2BPP] == 0 && vdu.aspect == 2)
        return 1;
    return idx[vdu.mv[MV_LOG2BPP]];
}

/* VDU 23,11: the default ECFs, BBC-compatible */
void vdu_ecf_default(void)
{
    static const uint32_t def[6][4] = {
        { 0, 0, 0, 0 },
        { 0x00330033, 0xCC33CC33, 0xCCFFCCFF, 0x030C30C0 },
        { 0x55665566, 0x99669966, 0x99AA99AA, 0xBBEEBBEE },
        { 0x31133113, 0x51155115, 0x32233223, 0x37733773 },
        { 0x00550055, 0xAA55AA55, 0xAAFFAAFF, 0x11224488 },
        { 0xFFFEFDFC, 0x00010203, 0x20212223, 0xDFDEDDDC },
    };
    const uint32_t *w = def[ecf_index()];
    vdu.bbc_ecfs = 0;
    for (int n = 0; n < 4; n++)
        for (int i = 0; i < 8; i++)
            vdu.ecf[n][i] = (uint8_t)(w[n] >> (8 * (i & 3)));
    vdu_set_colour();
}

/* VDU 23,2-5 define ECF n (0-3). The BBC bytes are de-interleaved unless
 * they are native. */
void vdu_ecf_complex(uint32_t n, const uint8_t b[8])
{
    /* InterleaveTB. At 16 and 32 bpp the kernel reads past the table into
     * the code after it (LineStyle). The last two rows here are the bytes
     * of that code in the ROM. */
    static const uint8_t tb[6][8] = {
        { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 },
        { 0x08, 0x80, 0x04, 0x40, 0x02, 0x20, 0x01, 0x10 },
        { 0x02, 0x08, 0x20, 0x80, 0x01, 0x04, 0x10, 0x40 },
        { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 },
        { 0x08, 0x30, 0xA0, 0xE3, 0x6C, 0x10, 0x8C, 0xE2 },
        { 0x72, 0x2F, 0x8C, 0xE2, 0x01, 0x00, 0x71, 0xE5 },
    };
    for (int i = 0; i < 8; i++) {
        uint32_t v = b[i];
        if (!vdu.bbc_ecfs) {
            const uint8_t *t = tb[vdu.mv[MV_LOG2BPP]];
            uint32_t out = 0;
            for (int k = 0; k < 8; k++)
                if (v & t[k])
                    out |= 1u << k;
            v = out;
        }
        vdu.ecf[n][i] = (uint8_t)v;
    }
    vdu_set_colour();
}

/* VDU 23,12-15 define ECF n (0-3) from colours. Below 8 bpp they are
 * pairs. Otherwise each byte is a GCOL colour with its tint. */
void vdu_ecf_simple(uint32_t n, const uint8_t b[8])
{
    static const uint8_t simp[6] = { 0x00, 0x33, 0x33, 0x0F, 0x55, 0xFF };
    uint32_t mask = simp[ecf_index()], nc = vdu.mv[MV_NCOLOUR];
    if (mask == 0xFF) {
        for (int i = 0; i < 8; i++)
            vdu.ecf[n][i] = (uint8_t)add_tint(b[i] & 0x3F, b[i] & 0xC0);
    } else {
        /* At 1 bpp the kernel reads the colours from address 0, because a
         * register is reused before its use. They are taken here as colour
         * 0. */
        uint8_t c[8];
        for (int i = 0; i < 8; i++)
            c[i] = vdu.mv[MV_LOG2BPP] == 0 ? 0 : b[i];
        for (int i = 0; i < 4; i++) {
            uint32_t e = c[2 * i] & nc, o = c[2 * i + 1] & nc, fe, fo;
            uint32_t mul = nc == 1 ? 0xFF : nc == 3 ? 0x55 : 0x11;
            fe = e * mul, fo = o * mul;
            uint8_t row = (uint8_t)((fe & mask) | (fo & ~mask));
            vdu.ecf[n][i] = vdu.ecf[n][i + 4] = row;
        }
    }
    vdu_set_colour();
}

/* VDU 23,17,6 and OS_SetECFOrigin: external co-ordinates */
void vdu_ecf_origin(int32_t x, int32_t y)
{
    int32_t xi = (x + vdu.orgx) >> vdu.mv[MV_XEIG], yi = (y + vdu.orgy) >> vdu.mv[MV_YEIG];
    uint32_t s = ((uint32_t)xi & plot_npix()) << vdu.mv[MV_LOG2BPC];
    vdu.ecf_shift = 32 - s;
    vdu.ecf_yoffset = (uint32_t)((int32_t)vdu.mv[MV_YWIND] + 1 - yi) & 7;
    vdu_set_colour();
}

void ros_vdu_ecf_offsets(uint32_t *shift, uint32_t *yoffset)
{
    *shift = vdu.ecf_shift, *yoffset = vdu.ecf_yoffset;
}

uint32_t ros_vdu_scratch(void)
{
    static uint32_t scratch;
    if (!scratch)
        scratch = ros_addr(ros_rma_alloc(256));
    return scratch;
}

/* ---- the changed box -------------------------------------------------------------------- */

/* ClipBoxEnable, ClipBoxLCol..TRow. These are in the RMA, because the
 * address is handed out and Draw, the Font Manager and the Wimp write the
 * box themselves. */
uint32_t *vdu_cbox(void)
{
    static uint32_t box;
    if (!box) {
        box = ros_addr(ros_rma_alloc(20));
        memset(ros_ptr(box), 0, 20);
    }
    return ros_ptr(box);
}

/* Merges a rectangle into the program's box while it is on, and into a
 * virtual display's own box whenever output goes to its screen
 * (VDU_CLIPPING). */
void vdu_cbox_merge(int32_t l, int32_t b, int32_t r, int32_t t)
{
    if (vdu.vd && !vdu.dest_sprite)
        vdisplay_dirty(l, b, r, t);
    if (!(vdu.cursor_flags & CF_CLIPBOX))
        return;
    int32_t *k = (int32_t *)vdu_cbox() + 1;
    if (l < k[0]) k[0] = l;
    if (b < k[1]) k[1] = b;
    if (r > k[2]) k[2] = r;
    if (t > k[3]) k[3] = t;
}

void vdu_cbox_points(const int32_t *xy, int n)
{
    int32_t l = xy[0], b = xy[1], r = l, t = b;
    for (int i = 1; i < n; i++) {
        int32_t x = xy[2 * i], y = xy[2 * i + 1];
        if (x < l) l = x;
        if (y < b) b = y;
        if (x > r) r = x;
        if (y > t) t = y;
    }
    if (l < vdu.gwl) l = vdu.gwl;
    if (b < vdu.gwb) b = vdu.gwb;
    if (r > vdu.gwr) r = vdu.gwr;
    if (t > vdu.gwt) t = vdu.gwt;
    if (r >= l && t >= b)
        vdu_cbox_merge(l, b, r, t);
}

void vdu_cbox_full(void)
{
    if (vdu.vd && !vdu.dest_sprite)
        vdisplay_dirty(0, 0, (int32_t)vdu.mv[MV_XWIND], (int32_t)vdu.mv[MV_YWIND]);
    if (!(vdu.cursor_flags & CF_CLIPBOX))
        return;
    int32_t *k = (int32_t *)vdu_cbox() + 1;
    k[0] = k[1] = 0;
    k[2] = (int32_t)vdu.mv[MV_XWIND], k[3] = (int32_t)vdu.mv[MV_YWIND];
}

void vdu_cbox_text(int32_t l, int32_t b, int32_t r, int32_t t)
{
    int32_t m = (int32_t)vdu.row_mult, y = (int32_t)vdu.mv[MV_YWIND];
    vdu_cbox_merge(l * 8, y - (b + 1) * m + 1, r * 8 + 7, y - t * m);
}

/* ClipCircle. The radius rad is in square pixels, and it is halved across
 * the long side. */
void vdu_cbox_circle(uint32_t rad, int32_t cx, int32_t cy)
{
    int32_t dx = (int32_t)(vdu.aspect == 1 ? rad >> 1 : rad);
    int32_t dy = (int32_t)(vdu.aspect > 1 ? rad >> 1 : rad);
    int32_t xy[4] = { cx - dx, cy - dy, cx + dx, cy + dy };
    vdu_cbox_points(xy, 2);
}

/* DoPlotClipBox. This gives what each group of PLOT codes adds. Kinds 1 to
 * 3 add the last so many points. Kind 4 adds the graphics window (flood
 * fills), 5 an ellipse's parallelogram, 6 a parallelogram and 7 a line
 * fill's row. Circles, block copies and sprites add their own as they
 * draw. */
static const uint8_t plot_cbox_kind[32] = {
    2, 2, 2, 2, 2, 2, 2, 2, 1, 7, 3, 7, 2, 7, 6, 7,
    4, 4, 0, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0,
};

static void parallel_cbox(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    int32_t xy[8] = { x0, y0, x1, y1, x2, y2, x0 + x2 - x1, y0 + y2 - y1 };
    vdu_cbox_points(xy, 4);
}

static void plot_cbox(uint32_t k)
{
    uint32_t kind = (k & 3) ? plot_cbox_kind[k >> 3] : 0;
    int32_t xy[6] = { vdu.oldx, vdu.oldy, vdu.gcsix, vdu.gcsiy, vdu.newptx, vdu.newpty };
    switch (kind) {
    case 1: case 2: case 3:
        vdu_cbox_points(xy + 6 - 2 * kind, (int)kind);
        break;
    case 4:
        vdu_cbox_merge(vdu.gwl, vdu.gwb, vdu.gwr, vdu.gwt);
        break;
    case 5: {                           /* A, B and C: the parallelogram round it */
        int32_t ax = xy[0], ay = xy[1], bx = xy[2], cx = xy[4], cy = xy[5], d = bx - ax;
        parallel_cbox(ax + bx - cx, 2 * ay - cy, cx + d, cy, cx - d, cy);
        break;
    }
    case 6:
        parallel_cbox(xy[0], xy[1], xy[2], xy[3], xy[4], xy[5]);
        break;
    case 7:
        if (vdu.newptx >= vdu.gwl && vdu.gwr >= vdu.newptx &&
            vdu.newpty >= vdu.gwb && vdu.gwt >= vdu.newpty)
            vdu_cbox_merge(vdu.gwl, vdu.newpty, vdu.gwr, vdu.newpty);
        break;
    default:
        break;
    }
}

/* OS_ChangedBox. R0 is 0 for off, 1 for on, or 2 to make the box null. It
 * returns the old state in R0 and the address of the five words in R1. */
void ros_thunk_OS_ChangedBox(struct ros_cpu *s)
{
    uint32_t *box = vdu_cbox(), old = box[0];
    if (s->r[0] <= 1) {
        box[0] = s->r[0];
        if (s->r[0] & 1)
            vdu.cursor_flags |= CF_CLIPBOX;
        else
            vdu.cursor_flags &= ~CF_CLIPBOX;
    } else if (s->r[0] == 2) {
        box[1] = box[2] = 0x7FFFFFFF;
        box[3] = box[4] = 0x80000000u;
    }
    s->r[0] = old, s->r[1] = ros_addr(box);
    s->v = 0;
}

/* ---- the dot-dash pattern ---------------------------------------------------------- */

static void default_line_style(void)
{
    vdu.dot_length = 8;
    vdu.dot_style = 0xAAAAAAAAAAAAAAAAull;
    vdu.dot_cnt = 0;
}

/* VDU 23,6. The first byte's top bit is the first pixel. */
void vdu_line_style(const uint8_t b[8])
{
    uint64_t p = 0;
    for (int i = 0; i < 8; i++)
        p = p << 8 | b[i];
    vdu.dot_style = p;
}

/* OS_Byte 163,242,n */
void vdu_dot_length(struct ros_cpu *s)
{
    uint32_t n = s->r[2];
    if (n == 0)
        default_line_style();
    else if (n <= 64)
        vdu.dot_length = n, vdu.dot_cnt = 0;
    else if (n == 65)
        s->r[1] = 0xC0 | (vdu.dot_length & 63), s->r[2] = 0;
    else if (n == 66)
        s->r[1] = s->r[2] = 0;
}

static int dot_step(void)
{
    if (vdu.dot_cnt == 0) {
        vdu.dot_pat = vdu.dot_style;
        vdu.dot_cnt = vdu.dot_length;
    }
    vdu.dot_cnt--;
    int on = (int)(vdu.dot_pat >> 63);
    vdu.dot_pat <<= 1;
    return on;
}

/* A mode's plotting defaults (SwitchOutputToSprite's). */
void vdu_plot_mode(void)
{
    int32_t xe = (int32_t)vdu.mv[MV_XEIG], ye = (int32_t)vdu.mv[MV_YEIG];
    vdu.aspect = xe > ye ? 1 : xe < ye ? 2 : 0;
    vdu.ecf_yoffset = (vdu.mv[MV_YWIND] + 1) & 7;
    vdu.ecf_shift = 0;
    default_line_style();
    vdu_ecf_default();                  /* and SetColour */
}

/* ---- lines ---------------------------------------------------------------------- */

void plot_gen_line(struct plot_line *l, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    l->ex = x1, l->ey = y1;
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t fudge = dy >= dx ? -1 : 0;
    l->sx = dx < 0 ? -1 : 1;
    if (dx < 0)
        dx = -dx;
    l->sy = dy < 0 ? -1 : 1;
    if (dy < 0)
        dy = -dy;
    int32_t m = dy - dx >= 0 ? dy : dx;
    l->bres = ((m + fudge) >> 1) - dy;
    l->dx = dx, l->dy = dy, l->x = x0, l->y = y0;
}

void plot_adv_line(struct plot_line *l)
{
    if (l->bres < 0) {
        l->y += l->sy;
        l->bres += l->dx;
        if (l->bres < 0)
            return;
    }
    l->bres -= l->dy;
    l->x += l->sx;
}

/* Groups 0-7 draw from the previous point to this one. */
static void line_draw(uint32_t k)
{
    int dotted = (k & 0x10) != 0;
    if (!dotted && (k & 3) == 0)
        return;
    int excl_first = (k & 0x20) != 0, excl_last = (k & 0x08) != 0;
    if (dotted && !excl_first)
        vdu.dot_cnt = 0;                /* restart the pattern */
    struct plot_line l;
    plot_gen_line(&l, vdu.gcsix, vdu.gcsiy, vdu.newptx, vdu.newpty);
    int32_t n = (l.dx > l.dy ? l.dx : l.dy) + 1;
    if (excl_first) {
        n--;
        plot_adv_line(&l);
    }
    if (excl_last)
        n--;
    for (int32_t i = 0; i < n; i++) {
        int on = dotted ? dot_step() : 1;
        if (on && plot_in_window(l.x, l.y))
            plot_write(l.x, l.y);
        if (i < n - 1)
            plot_adv_line(&l);
    }
}

/* ---- line fills and flood fill ------------------------------------------------------- */

static const uint32_t *fill_colour;     /* FgEcf or BgEcf */
static int fill_over;                   /* 1 fills over that colour, 0 fills up to it */

static int fillable(int32_t x, int32_t y)
{
    uint32_t w = *plot_word(x, y), t = fill_colour[plot_erow(y)];
    int eq = ((w ^ t) & plot_pmask(x)) == 0;
    return fill_over ? eq : !eq;
}

static int32_t fill_right(int32_t x, int32_t y)
{
    int32_t p = x;
    while (p <= vdu.gwr && fillable(p, y))
        p++;
    plot_new_hline(x, y, p - 1);
    return p - 1;
}

static int32_t fill_left(int32_t x, int32_t y)
{
    int32_t p = x - 1;
    while (p >= vdu.gwl && fillable(p, y))
        p--;
    if (p + 1 <= x - 1)
        plot_new_hline(p + 1, y, x - 1);
    return p + 1;
}

/* FillAlong. It returns 1 if nothing was filled. */
static int fill_along(int32_t x, int32_t y, int32_t *l, int32_t *r)
{
    if (!plot_in_window(x, y) || !fillable(x, y)) {
        *l = *r = x;
        return 1;
    }
    *r = fill_right(x, y);
    *l = fill_left(x, y);
    return 0;
}

/* IEGB. This sets the external cursor from an internal point. */
static void iegb(int32_t x, int32_t y)
{
    vdu.gcsx = (int32_t)((uint32_t)x << vdu.mv[MV_XEIG]) - vdu.orgx;
    vdu.gcsy = (int32_t)((uint32_t)y << vdu.mv[MV_YEIG]) - vdu.orgy;
}

void vdu_ieg(void)
{
    iegb(vdu.gcsix, vdu.gcsiy);
}

/* PLOT 72-79 and 104-111 fill left and right. */
static void fill_lr(const uint32_t *col, int over)
{
    fill_colour = col, fill_over = over;
    int32_t x = vdu.newptx, y = vdu.newpty, l, r;
    if (fill_along(x, y, &l, &r)) {
        vdu.gcsix = x, vdu.gcsiy = y - 1;
        return;
    }
    vdu.gcsix = l, vdu.gcsiy = y;
    vdu.newptx = r;
    iegb(r, y);
}

/* PLOT 88-95 and 120-127 fill to the right only. */
static void fill_r(const uint32_t *col, int over)
{
    fill_colour = col, fill_over = over;
    int32_t x = vdu.newptx, y = vdu.newpty, r;
    vdu.gcsix = x, vdu.gcsiy = y;
    if (!plot_in_window(x, y) || !fillable(x, y))
        r = x - 1;
    else
        r = fill_right(x, y);
    vdu.newptx = r;
    iegb(r, y);
}

/* Flood fill uses a FIFO of spans. Each span is to be looked beyond in one
 * direction. */
enum { UP = 1, DOWN = 2 };
#define QUEUE_SLOTS 16384               /* 256K of 16-byte entries: 16383 outstanding */

struct span {
    int32_t l, r, y;
    int dir;
};
static struct span *queue;
static uint32_t q_head, q_tail;
static int32_t fld_l, fld_r;

static int enqueue(int32_t l, int32_t r, int32_t y, int dir)
{
    queue[q_tail] = (struct span){ l, r, y, dir };
    uint32_t t = (q_tail + 1) % QUEUE_SLOTS;
    if (t == q_head)
        return -1;                      /* full, so the flood stops here */
    q_tail = t;
    return 0;
}

/* CheckAlong. This checks the row beyond a span, in one direction. It
 * returns -1 to stop. */
static int check_along(int32_t lx, int32_t y, int dir)
{
    int32_t y2 = dir == UP ? y + 1 : y - 1;
    int other = dir == UP ? DOWN : UP;
    int32_t lxlim = fld_l, rxlim = fld_r, x = lx, left, p;
    if (!fillable(lx, y2))
        goto skip;
    left = fill_left(lx, y2);
    x = lx;
fill:
    p = x;
    while (p <= vdu.gwr && fillable(p, y2))
        p++;
    plot_new_hline(x, y2, p - 1);
    {
        int32_t right = p - 1;
        if (enqueue(left, right, y2, dir))
            return -1;
        if (left <= lxlim - 2 && enqueue(left, lxlim - 2, y2, other))
            return -1;
        if (right >= rxlim + 2 && enqueue(rxlim + 2, right, y2, other))
            return -1;
        if (right >= rxlim - 1)
            return 0;
    }
    x = p;
skip:
    for (;;) {
        if (fillable(x, y2)) {
            left = x;
            goto fill;
        }
        if ((uint32_t)x >= (uint32_t)rxlim)
            return 0;
        x++;
    }
}

static void flood(const uint32_t *col, int over)
{
    fill_colour = col, fill_over = over;
    int32_t l, r, y = vdu.newpty;
    if (fill_along(vdu.newptx, y, &l, &r))
        return;
    if (!queue)
        queue = malloc(QUEUE_SLOTS * sizeof *queue);
    if (!queue)
        return;
    q_head = q_tail = 0;
    int dir = UP | DOWN;
    for (uint32_t guard = 0; guard < 100000000u; guard++) {   /* the kernel has no guard */
        fld_l = l, fld_r = r;
        if ((dir & UP) && y < vdu.gwt && check_along(l, y, UP))
            return;
        if ((dir & DOWN) && y > vdu.gwb && check_along(l, y, DOWN))
            return;
        if (q_head == q_tail)
            return;
        struct span s = queue[q_head];
        q_head = (q_head + 1) % QUEUE_SLOTS;
        l = s.l, r = s.r, y = s.y, dir = s.dir;
    }
}

/* ---- triangles, rectangles, parallelograms ---------------------------------------- */

struct pt {
    int32_t x, y;
};

/* CompSwapT. This sorts by ascending y, and by descending x for equal y. */
static void compswap(struct pt *a, struct pt *b)
{
    if (a->y < b->y || (a->y == b->y && a->x >= b->x))
        return;
    struct pt t = *a;
    *a = *b, *b = t;
}

static struct plot_line tl1, tl2;
static int32_t tend_y;
static struct pt vtx[4];

static void trap_step(struct plot_line *l, int32_t *lo, int32_t *hi)
{
    if (l->ey == l->y)
        l->x = l->ex;
    else
        while (l->bres >= 0) {
            l->x += l->sx;
            l->bres -= l->dy;
        }
    if (l->x < *lo)
        *lo = l->x;
    if (l->x > *hi)
        *hi = l->x;
}

static void trap_fill(int32_t *lo, int32_t *hi)
{
    for (;;) {
        trap_step(&tl1, lo, hi);
        trap_step(&tl2, lo, hi);
        int32_t y = tl2.y;
        if (tend_y == y)
            return;
        plot_new_hline(*lo, y, *hi);
        plot_adv_line(&tl1);
        *lo = tl1.x;
        plot_adv_line(&tl2);
        if (tl2.x >= *lo)
            *hi = tl2.x;
        else
            *hi = *lo, *lo = tl2.x;
    }
}

static void lower_tri(struct pt a, struct pt b, struct pt c, int32_t *lo, int32_t *hi)
{
    compswap(&a, &b);
    compswap(&b, &c);
    compswap(&a, &b);
    vtx[0] = a, vtx[1] = b, vtx[2] = c;
    *lo = *hi = a.x;
    plot_gen_line(&tl1, a.x, a.y, b.x, b.y);
    tend_y = b.y;
    plot_gen_line(&tl2, a.x, a.y, c.x, c.y);
    trap_fill(lo, hi);
}

static void triangle(void)
{
    int32_t lo, hi;
    lower_tri((struct pt){ vdu.oldx, vdu.oldy }, (struct pt){ vdu.gcsix, vdu.gcsiy },
              (struct pt){ vdu.newptx, vdu.newpty }, &lo, &hi);
    plot_gen_line(&tl1, vtx[1].x, vtx[1].y, vtx[2].x, vtx[2].y);
    tend_y = vtx[2].y;
    trap_fill(&lo, &hi);
    plot_new_hline(lo, tl2.y, hi);
}

static void parallelogram(void)
{
    struct pt p0 = { vdu.oldx, vdu.oldy }, p1 = { vdu.gcsix, vdu.gcsiy },
              p2 = { vdu.newptx, vdu.newpty };
    struct pt p3 = { vdu.oldx + vdu.newptx - vdu.gcsix, vdu.oldy + vdu.newpty - vdu.gcsiy };
    compswap(&p0, &p1);
    compswap(&p1, &p2);
    compswap(&p2, &p3);
    int32_t lo, hi;
    lower_tri(p0, p1, p2, &lo, &hi);
    vtx[3] = p3;
    tend_y = vtx[2].y;
    plot_gen_line(&tl1, vtx[1].x, vtx[1].y, vtx[3].x, vtx[3].y);
    trap_fill(&lo, &hi);
    plot_gen_line(&tl2, vtx[2].x, vtx[2].y, vtx[3].x, vtx[3].y);
    tend_y = vtx[3].y;
    trap_fill(&lo, &hi);
    plot_new_hline(lo, tl2.y, hi);
}

/* ---- block copy and move ----------------------------------------------------------- */

struct box {
    int32_t l, b, r, t;
};

/* EraseDifference. This erases the big box less the small one, which
 * shares an edge each way. */
static void erase_difference(struct box big, struct box small)
{
    if (small.r < small.l || small.t < small.b) {
        plot_rect(big.l, big.b, big.r, big.t);
        return;
    }
    struct box r = big;
    if (big.t == small.t)
        r.t = small.b - 1;
    else
        r.b = small.t + 1;
    if (r.t >= r.b)
        plot_rect(r.l, r.b, r.r, r.t);
    r = big;
    if (big.t == small.t)
        r.b = small.b;
    else
        r.t = small.t;
    if (big.l == small.l)
        r.l = small.r + 1;
    else
        r.r = small.l - 1;
    if (r.t >= r.b && r.r >= r.l)
        plot_rect(r.l, r.b, r.r, r.t);
}

static void block_copy_move(uint32_t k)
{
    if ((k & 3) == 0)
        return;
    int copy = (k & 2) != 0;
    plot_gcol = vdu.bg_store;
    struct box src = { vdu.oldx < vdu.gcsix ? vdu.oldx : vdu.gcsix,
                       vdu.oldy < vdu.gcsiy ? vdu.oldy : vdu.gcsiy,
                       vdu.oldx > vdu.gcsix ? vdu.oldx : vdu.gcsix,
                       vdu.oldy > vdu.gcsiy ? vdu.oldy : vdu.gcsiy };
    struct box dst = { vdu.newptx, vdu.newpty, src.r - src.l + vdu.newptx,
                       src.t - src.b + vdu.newpty };
    if (VDU_CLIPPING) {       /* ClipBlockCopyMove, including the move's source */
        int32_t xy[8] = { src.l, src.b, src.r, src.t, dst.l, dst.b, dst.r, dst.t };
        vdu_cbox_points(copy ? xy + 4 : xy, copy ? 2 : 4);
    }
    struct box s = src, d = dst, d2, d3;
    int32_t n;
    if ((n = vdu.gwl - d.l) > 0) d.l += n, s.l += n;
    if ((n = d.r - vdu.gwr) > 0) d.r -= n, s.r -= n;
    if (d.r < d.l)
        goto erase_source;
    d2.l = d.l, d2.r = d.r;
    if ((n = vdu.gwl - s.l) > 0) s.l += n, d.l += n;
    if ((n = s.r - vdu.gwr) > 0) s.r -= n, d.r -= n;
    if ((n = vdu.gwb - d.b) > 0) d.b += n, s.b += n;
    if ((n = d.t - vdu.gwt) > 0) d.t -= n, s.t -= n;
    if (d.t < d.b)
        goto erase_source;
    d2.b = d.b, d2.t = d.t;
    if ((n = vdu.gwb - s.b) > 0) s.b += n, d.b += n;
    if ((n = s.t - vdu.gwt) > 0) s.t -= n, d.t -= n;
    d3 = d;
    if (!(s.r < s.l || s.t < s.b)) {
        uint32_t w = (uint32_t)(s.r - s.l + 1), h = (uint32_t)(s.t - s.b + 1);
        uint32_t *buf = malloc((size_t)w * h * sizeof *buf);
        if (buf) {
            for (uint32_t j = 0; j < h; j++)
                for (uint32_t i = 0; i < w; i++)
                    buf[j * w + i] = get_pixel(s.l + (int32_t)i, s.b + (int32_t)j) >>
                                     (((uint32_t)(s.l + (int32_t)i) & plot_npix()) * vdu.bpc % 32);
            for (uint32_t j = 0; j < h; j++)
                for (uint32_t i = 0; i < w; i++) {
                    int32_t x = d.l + (int32_t)i;
                    uint32_t sh = ((uint32_t)x & plot_npix()) * vdu.bpc % 32;
                    put_pixel(x, d.b + (int32_t)j, buf[j * w + i] << sh);
                }
            free(buf);
        }
    }
    erase_difference(d2, d3);
erase_source:
    if (copy)
        return;
    s = src, d = dst;
    if ((n = vdu.gwl - s.l) > 0) s.l += n, d.l += n;
    if ((n = s.r - vdu.gwr) > 0) s.r -= n, d.r -= n;
    if (s.r < s.l)
        return;
    if ((n = vdu.gwb - s.b) > 0) s.b += n, d.b += n;
    if ((n = s.t - vdu.gwt) > 0) s.t -= n, d.t -= n;
    if (s.t < s.b)
        return;
    if (d.t >= s.t) d.t = s.t; else d.b = s.b;
    if (d.r >= s.r) d.r = s.r; else d.l = s.l;
    erase_difference(s, d);
}

/* ---- PLOT ---------------------------------------------------------------------------- */

static void ctidy(void)
{
    vdu.olderx = vdu.oldx, vdu.oldery = vdu.oldy;
    vdu.oldx = vdu.gcsix, vdu.oldy = vdu.gcsiy;
    vdu.gcsix = vdu.newptx, vdu.gcsiy = vdu.newpty;
}

/* UKPLOTV for the groups that nothing here draws. It returns its error, if
 * any. */
static os_error *ukplot(uint32_t k)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = k, s.r[1] = vdu.qq;
    ros_vector_call(UKPLOTV, &s);
    return s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
}

os_error *vdu_plot(uint32_t k, int32_t x, int32_t y)
{
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC)
        return NULL;
    k &= 0xFF;
    if (!(k & 4))
        x += vdu.gcsx, y += vdu.gcsy;
    vdu.gcsx = x, vdu.gcsy = y;
    vdu.newptx = (x + vdu.orgx) >> vdu.mv[MV_XEIG];
    vdu.newpty = (y + vdu.orgy) >> vdu.mv[MV_YEIG];
    static const uint32_t *const tables[4] = { no_effect, NULL, invert_tbl, NULL };
    plot_gcol = (k & 3) == 1 ? vdu.fg_oe : (k & 3) == 3 ? vdu.bg_oe : tables[k & 3];
    if (VDU_CLIPPING)
        plot_cbox(k);
    uint32_t g = k >> 3;
    if (g >= 26 && g != 29) {                   /* UKPLOTV */
        os_error *e = ukplot(k);
        if (!e)                         /* after an error there is no shuffle, and the error is the VDU's */
            ctidy();
        return e;
    }
    if (vdu.screen_ok) {
        switch (g) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            line_draw(k);
            break;
        case 8: plot_point(vdu.newptx, vdu.newpty); break;
        case 9: fill_lr(vdu.bg_ecf, 1); break;
        case 10: triangle(); break;
        case 11: fill_r(vdu.bg_ecf, 0); break;
        case 12: plot_rect(vdu.gcsix, vdu.gcsiy, vdu.newptx, vdu.newpty); break;
        case 13: fill_lr(vdu.fg_ecf, 0); break;
        case 14: parallelogram(); break;
        case 15: fill_r(vdu.fg_ecf, 1); break;
        case 16: flood(vdu.bg_ecf, 1); break;
        case 17: flood(vdu.fg_ecf, 0); break;
        case 18: plot_circle_outline(); break;
        case 19: plot_circle_fill(); break;
        case 20: plot_arc(); break;
        case 21: plot_segment(); break;
        case 22: plot_sector(); break;
        case 23: block_copy_move(k); break;
        case 24: plot_ellipse(0); break;
        case 25: plot_ellipse(1); break;
        case 29: vdu_plot_sprite(k); break;
        default: break;
        }
    }
    ctidy();
    return NULL;
}

/* VDU 16 clears the graphics window, with the background colour and
 * action. */
void vdu_clg(void)
{
    if (vdu.mv[MV_FLAGS] & MF_NONGRAPHIC)
        return;
    if (VDU_CLIPPING)
        vdu_cbox_merge(vdu.gwl, vdu.gwb, vdu.gwr, vdu.gwt);
    if (!vdu.screen_ok)
        return;
    plot_gcol = vdu.bg_oe;
    plot_rect(vdu.gwl, vdu.gwb, vdu.gwr, vdu.gwt);
}

/* ExportedHLine, the routine at VDU variable HLineAddr. The ends may be in
 * either order. The parameter how is 0 for no effect, 1 for the foreground
 * colour and action, 2 for invert, 3 for the background, or else the
 * address of eight {ora, eor} pairs. */
void ros_vdu_hline(int32_t a, int32_t y, int32_t b, uint32_t how)
{
    static const uint32_t *const tables[4] = { no_effect, NULL, invert_tbl, NULL };
    plot_gcol = how == 1 ? vdu.fg_oe : how == 3 ? vdu.bg_oe
              : how < 4 ? tables[how] : (const uint32_t *)ros_ptr(how);
    if (vdu.screen_ok)
        plot_hline(a, y, b);
}

/* ---- the SWIs --------------------------------------------------------------------------- */

void ros_thunk_OS_Plot(struct ros_cpu *s)
{
    s->v = 0;
    if (s->r[0] >= 256)
        return;
    uint8_t b[6] = { 25, (uint8_t)s->r[0], (uint8_t)s->r[1], (uint8_t)(s->r[1] >> 8),
                     (uint8_t)s->r[2], (uint8_t)(s->r[2] >> 8) };
    for (int i = 0; i < 6; i++) {
        os_error *e = xos_write_c(b[i]);
        if (e) {
            ros_swi_fail(s, e);
            return;
        }
    }
}

void ros_thunk_OS_ReadPoint(struct ros_cpu *s)
{
    int32_t x = (int32_t)s->r[0], y = (int32_t)s->r[1];
    s->v = 0;
    int32_t xi = (x + vdu.orgx) >> vdu.mv[MV_XEIG], yi = (y + vdu.orgy) >> vdu.mv[MV_YEIG];
    if ((vdu.mv[MV_FLAGS] & MF_NONGRAPHIC) || !vdu.screen_ok || !plot_in_window(xi, yi)) {
        s->r[2] = 0xFFFFFFFFu, s->r[3] = 0, s->r[4] = 0xFFFFFFFFu;
        return;
    }
    vdu_pre_wrch();
    uint32_t word = *plot_word(xi, yi), nc = vdu.mv[MV_NCOLOUR];
    uint32_t msk = nc == 63 ? 255 : nc;
    uint32_t sh = ((uint32_t)xi & plot_npix()) << vdu.mv[MV_LOG2BPC];
    uint32_t pix = (sh >= 32 ? 0 : word >> sh) & msk;
    if (msk == 255) {
        s->r[3] = (pix << 6) & 0xC0;
        s->r[2] = ((pix & 0x84) | (pix & 8 ? 0x40 : 0) | ((pix & 0x70) >> 1)) >> 2;
    } else {
        s->r[2] = pix, s->r[3] = 0;
    }
    s->r[4] = 0;
    vdu_post_wrch();
}

void ros_thunk_OS_SetColour(struct ros_cpu *s)
{
    uint32_t f = s->r[0];
    s->v = 0;
    int bg = (f & 0x10) != 0;
    if (f & 0x80) {                                     /* read */
        if (f & 0x40) {
            s->r[1] = bg ? vdu.text_bg : vdu.text_fg;
            s->r[0] = f & ~0xA0u;
        } else {
            memcpy(ros_ptr(s->r[1]), bg ? vdu.bg_ecf : vdu.fg_ecf, 32);
            s->r[0] = ((bg ? vdu.gplbmd : vdu.gplfmd) & 0x0F) | (bg ? 0x10 : 0) | 0x20;
        }
        return;
    }
    if (f & 0x40) {                                     /* text */
        if (bg)
            vdu.text_bg = s->r[1];
        else
            vdu.text_fg = s->r[1];
        vdu.cursor_flags |= CF_TEUPDATE;
        return;
    }
    uint32_t op = (f & 0x0F) | 0x60, *pat = bg ? vdu.bg_pattern : vdu.fg_pattern;
    if (bg)
        vdu.gplbmd = op;
    else
        vdu.gplfmd = op;
    if (f & 0x20) {
        memcpy(pat, ros_ptr(s->r[1]), 32);
    } else {
        uint32_t bpp = vdu.bpp, w = s->r[1] & (bpp == 32 ? ~0u : (1u << bpp) - 1);
        for (uint32_t sft = bpp; sft < 32; sft <<= 1)
            w |= w << sft;
        for (int i = 0; i < 8; i++)
            pat[i] = w;
    }
    vdu_set_colour();
}

void ros_thunk_OS_SetECFOrigin(struct ros_cpu *s)
{
    s->v = 0;
    int32_t gx = vdu.gcsx, gy = vdu.gcsy;
    vdu_ecf_origin((int32_t)s->r[0], (int32_t)s->r[1]);
    vdu.gcsx = gx, vdu.gcsy = gy;
}
