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
 * (Sources/Kernel: s.vdu.vduttx, Docs.HiResTTX).
 */
/* ttx.c -- teletext: MODE 7, and any mode whose flags say teletext.
 *
 * This follows the kernel's Kernel/s/vdu/vduttx, routine for routine, as
 * RISC OS 5 builds it for a Pi (HiResTTX and TTX256, Docs/HiResTTX). The
 * kernel's names are in the comments.
 *
 *   - The page is a map with a word for each character cell (TTXMap). A
 *     word holds the character and the attributes in force after it. The
 *     attributes are alphanumerics or graphics, contiguous or separated,
 *     flash, hold, conceal, and single or double height (and whether the
 *     row is a double row's bottom half). They also include the pending
 *     start and end of a box, the foreground and background colours,
 *     whether the cell is boxed, and the held graphic (MapBit_*). Each row
 *     starts with a dummy word, whose attributes are the row's defaults. A
 *     count of the double-height codes on each row goes with it
 *     (TTXDoubleCounts).
 *   - Writing a character stores it in the map and paints it. It then goes
 *     on along the row, painting what follows for as long as the
 *     attributes that reach it change. Then it goes down, while a row's
 *     double-height codes make the next row a bottom half or stop doing so
 *     (TTXScanZap). Clearing, scrolling and VDU 23,18 go through the same
 *     scan. When scrolling, the map moves and then the window is
 *     rescanned.
 *   - Control codes (&00-&1F and &80-&9F) act "at" the cell or "after" it.
 *     The codes that act at the cell are steady, conceal, the heights,
 *     black and new background, hold, and the start and end of a box. The
 *     codes that act after it are the colours, flash, contiguous and
 *     separated, and release (DoPreControl and DoPostControl). A control
 *     code shows as a space, or as the held graphic while hold is on.
 *   - Characters are 16 x 20 pixels. Letters come from the kernel's
 *     teletext font (ttxfont.c, made from vduttx by tools/mkttxfont.py).
 *     In it, #, _ and ` are swapped round to teletext's hash, long dash and
 *     pound, and they are swapped back when OS_Byte 135 reads one. Mosaics
 *     are made as each is drawn (ComputeGraphic). They are six blocks of 8
 *     x 6, 8 x 8 and 8 x 6. Separated blocks are two pixels narrower and
 *     two rows shorter. Double height draws each font row twice, with the
 *     bottom half from row 10.
 *   - The screen is two banks, each ScreenSize / 2. The second has the
 *     flashing characters as spaces. The VSync shows each in turn, for 48
 *     and 16 VSyncs, by moving the display's start (TeletextFlashTest:
 *     GraphicsV SetDMAAddress). The cursor is drawn in both banks.
 *   - The colours are the eight teletext colours as pixel values 0-7. The
 *     values 8-15 are the same colours transparent, in VDU 23,18's other
 *     modes. The palette is palette.c's paldatT. Above 8 bpp each colour is
 *     ColourTrans's colour number for it. TForeCol and TBackCol are the
 *     colours last painted with, as the kernel's are (TTXUpdateColours).
 *   - VDU 23,18,0-3 set the transparency mode, suspend drawing, reveal, and
 *     enable black.
 *
 * The map is private to the VDU drivers, so it is the host's memory, not
 * the arena's, as the kernel's is its system heap's.
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "vduws.h"

/* Teletext control codes (TTX_*) */
enum {
    TTX_FLASH = 0x08, TTX_STEADY = 0x09, TTX_END_BOX = 0x0A, TTX_START_BOX = 0x0B,
    TTX_NORMAL_HEIGHT = 0x0C, TTX_DOUBLE_HEIGHT = 0x0D, TTX_CONCEAL = 0x18,
    TTX_CONTIGUOUS = 0x19, TTX_SEPARATED = 0x1A, TTX_BLACK_BACKGD = 0x1C, TTX_NEW_BACKGD = 0x1D,
    TTX_HOLD_GRAPH = 0x1E, TTX_REL_GRAPH = 0x1F,
};

/* The map's bits (MapBit_*): 0-7 the character */
#define MB_GRAPH            (1u << 8)
#define MB_SEPARATED        (1u << 9)
#define MB_FLASH            (1u << 10)
#define MB_HOLD             (1u << 11)
#define MB_CONCEAL          (1u << 12)
#define MB_BOTTOM           (1u << 13)
#define MB_DOUBLE           (1u << 14)
#define MB_PENDING_START    (1u << 15)
#define MB_PENDING_END      (1u << 16)
#define MB_FORE_SHIFT       17              /* 17-19; 20 boxed */
#define MB_BACK_SHIFT       21              /* 21-23; 24 boxed */
#define MB_HELD_SHIFT       25              /* 25-29, 31; 30 separated */
#define MB_FORE_MASK        (7u << MB_FORE_SHIFT)
#define MB_BACK_MASK        (7u << MB_BACK_SHIFT)
#define MB_BOXED            ((1u << 20) | (1u << 24))
#define MB_HELD_MASK        (0x7Fu << MB_HELD_SHIFT)
#define MB_HELD_SEPARATED   (1u << 30)
#define MB_DEFAULT          ((7u << MB_FORE_SHIFT) + 32)    /* a row's start */

/* TTXFlags are VDU 23,18's settings. The EOR and BIC masks act on the
 * map's colour bits, with EOR shifted up 8. */
#define TF_SUSPEND          (1u << 0)
#define TF_CONCEAL          (1u << 1)
#define TF_BLACK_ENABLE     (1u << 2)
#define TF_TRANS_SHIFT      3
#define TF_TRANS_MASK       (3u << TF_TRANS_SHIFT)
#define TF_FG_TRANS_EOR     (1u << 12)
#define TF_BG_TRANS_EOR     (1u << 16)
#define TF_FG_TRANS_BIC     (1u << 20)
#define TF_BG_TRANS_BIC     (1u << 24)

#define ON_FLASH_TIME   48      /* VSyncs with the flashing characters shown */
#define OFF_FLASH_TIME  16      /* and hidden */

#define GV_SET_DMA_ADDRESS 6u

extern const uint16_t ros_vdu_ttx_font[96][20];

static struct {
    uint32_t *map;          /* TTXMap: rows of (ScrRCol + 2) words, a dummy first */
    uint8_t *dbl;           /* TTXDoubleCounts: double-height codes on each row */
    uint32_t stride;        /* words a row */
    uint32_t rows;
    uint32_t flags;         /* TTXFlags */
    uint32_t count;         /* TeletextCount: VSyncs to the next flash */
    uint32_t offset;        /* TeletextOffset: the bank on show, 0 or ScreenSize / 2 */
} ttx;

/* The eight colours as ColourTrans words (TTXPalette_Solid). A colour's
 * number above 7 reads on into TTXPalette_Mixed, which is the same eight
 * again. */
static const uint32_t solid[8] = {
    0x00000000u, 0x0000FF00u, 0x00FF0000u, 0x00FFFF00u,
    0xFF000000u, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFF00u,
};

static uint32_t cols(void)
{
    return vdu.mv[MV_SCRRCOL] + 1;
}

/* The map index of column x's word on row y. Column -1 is the row's dummy
 * word. */
static int32_t entry(int32_t x, int32_t y)
{
    return y * (int32_t)ttx.stride + 1 + x;
}

/* ---- setting up ------------------------------------------------------------ */

/* TeletextInit. With alloc set it allocates for a new teletext mode: the
 * map (TeletextAlloc), the rows' starts and their counts. Otherwise it sets
 * only the settings, as when output comes back to a teletext screen. */
void vdu_ttx_init(int alloc)
{
    if (alloc) {
        vdu_ttx_final();
        ttx.stride = vdu.mv[MV_SCRRCOL] + 2;
        ttx.rows = vdu.mv[MV_SCRBROW] + 1;
        /* One word more is allocated. The scan reads the word after a row's
         * last as it stops. Past the last row, that word is the kernel's
         * TTXLineStarts. */
        ttx.map = calloc((size_t)ttx.stride * ttx.rows + 1, sizeof *ttx.map);
        ttx.dbl = calloc(ttx.rows, 1);
        ttx.offset = 0;
    }
    ttx.count = 1;                      /* it flashes at the next VSync */
    ttx.flags = TF_CONCEAL | TF_FG_TRANS_BIC | TF_BG_TRANS_BIC;
}

/* A context's own teletext (vdisplay.c). The state is copied out and in,
 * and a kept copy's map is freed. */
_Static_assert(sizeof ttx <= VDU_TTX_STATE, "VDU_TTX_STATE");

void vdu_ttx_state_save(void *to)
{
    memcpy(to, &ttx, sizeof ttx);
}

void vdu_ttx_state_load(const void *from)
{
    memcpy(&ttx, from, sizeof ttx);
}

void vdu_ttx_state_free(void *kept)
{
    __typeof__(ttx) t;
    memcpy(&t, kept, sizeof t);
    free(t.map);
    free(t.dbl);
    memset(kept, 0, sizeof t);
}

/* TeletextFinalise */
void vdu_ttx_final(void)
{
    free(ttx.map);
    free(ttx.dbl);
    ttx.map = NULL;
    ttx.dbl = NULL;
    ttx.offset = 0;
}

static int live(void)
{
    return ttx.map && (vdu.cursor_flags & CF_TELETEXT);
}

/* ---- colours ----------------------------------------------------------------- */

/* GetAlphaSupremacyBits' mask. At 64K and in packed 24 bpp it leaves the
 * background's ColourTrans word in its register, and the kernel uses
 * that. */
static uint32_t alpha_mask(uint32_t back)
{
    uint32_t nc = vdu.mv[MV_NCOLOUR];
    if (nc == 0xFFFFFFFFu)
        return 0xFF000000u;
    if (nc == 0x00FFFFFFu)
        return solid[back & 7];
    if (nc == 0xFFF)
        return 0xF000;
    if (!(vdu.mv[MV_FLAGS] & MF_FULLPALETTE))
        return 0x8000;
    return solid[back & 7];
}

static uint32_t colour_number(uint32_t word)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = word;
    ros_swi(&s, XColourTrans_ReturnColourNumber);
    return s.r[0];
}

/* TTXUpdateColours. This sets the colours to paint with, TForeCol and
 * TBackCol. */
static void update_colours(uint32_t fore, uint32_t back)
{
    vdu.tfore = fore, vdu.tback = back;
    if (vdu.bpp <= 8) {
        vdu.text_fg = fore, vdu.text_bg = back;
    } else {
        uint32_t f = colour_number(solid[fore & 7]), b = colour_number(solid[back & 7]);
        if (((fore | back) & 8) && (ttx.flags & TF_TRANS_MASK)) {
            uint32_t m = alpha_mask(back);
            if (fore & 8)
                f ^= m;
            if (back & 8)
                b ^= m;
        }
        vdu.text_fg = f, vdu.text_bg = b;
    }
    vdu_set_colours();
}

/* ---- painting ------------------------------------------------------------------ */

/* ComputeGraphic. This makes a mosaic as twenty rows, with bit 15
 * leftmost. Bits 0-4 and 6 of c are the blocks, from top left to bottom
 * right, and bit 5 means separated. */
static void compute_graphic(uint32_t c, uint16_t g[20])
{
    static const uint8_t height[3] = { 6, 8, 6 };
    static const uint8_t left[3] = { 0, 2, 4 }, right[3] = { 1, 3, 6 };
    int sep = (c & 0x20) != 0, r = 0;
    for (int band = 0; band < 3; band++) {
        uint16_t v = (uint16_t)((c >> left[band] & 1 ? 0xFF00 : 0) |
                                (c >> right[band] & 1 ? 0x00FF : 0));
        if (sep)
            v &= (uint16_t)~0xC0C0u;
        for (int i = 0; i < height[band]; i++, r++)
            g[r] = sep && i >= height[band] - 2 ? 0 : v;
    }
}

/* Paints sixteen pixels in the text colours, with bit 15 of bits leftmost.
 * Each pixel comes from its lane of the colour words, as the kernel's
 * TextExpand table words give it. At 64K the lanes differ when a
 * transparency mask put bits above the pixel's into the colour number
 * (SetColours, text.c). At 4 bpp (WrchHiResTTX4) each half is a table word,
 * and pixel x is its nibble x & 7. */
static void put16(uint8_t *p, uint32_t bits)
{
    uint32_t fg = vdu.text_fg, bg = vdu.text_bg, bpp = vdu.bpp;
    if (bpp >= 8) {
        uint32_t n = bpp / 8;
        for (uint32_t x = 0; x < 16; x++) {
            uint32_t v = bits >> (15 - x) & 1 ? fg : bg;
            v >>= 8 * (x * n & 3);                  /* the pixel's lane of the word */
            memcpy(p + x * n, &v, n);               /* little-endian: the low bytes */
        }
        return;
    }
    uint32_t pm = (1u << bpp) - 1;
    for (uint32_t x = 0; x < 16; x++) {
        uint32_t at = x * bpp, v = (bits >> (15 - x) & 1 ? fg : bg) >> (at & 31) & pm;
        p[at / 8] = (uint8_t)((p[at / 8] & ~(pm << at % 8)) | v << at % 8);
    }
}

static uint8_t *cell(int32_t col, int32_t row)
{
    return vdu.screen + vdu.text_offset + (uint32_t)row * vdu.row_length +
           (uint32_t)col * vdu.char_width;
}

/* TTXPaintChar. This paints a map word into its cell, in both banks. */
static void paint(uint32_t w, int32_t col, int32_t row)
{
    uint32_t f = ttx.flags;
    uint32_t a = (w & ~f) ^ (f << 8);           /* the BIC masks, then the EOR ones */
    uint32_t fore = (a >> MB_FORE_SHIFT) & 15, back = (a >> MB_BACK_SHIFT) & 15;
    if (vdu.tfore != fore || vdu.tback != back)
        update_colours(fore, back);
    if (!vdu.screen_ok)
        return;

    uint32_t c = w & 0x7F;
    int graphic = 0;
    if (c & 0x20) {                             /* &20-&3F, &60-&7F */
        if (w & MB_GRAPH) {
            graphic = 1;
            if (!(w & MB_SEPARATED))
                c &= ~0x20u;
        }
    } else if (!(c & 0x40)) {                   /* a control code shows as a space, */
        if (w & MB_HOLD) {                      /* or as the held graphic */
            graphic = 1;
            c = w >> MB_HELD_SHIFT;
        } else {
            c = 0x20;
        }
    }                                           /* &40-&5F: always a letter */
    uint16_t made[20];
    const uint16_t *g = ros_vdu_ttx_font[0];
    if (graphic) {
        if (c & 0x5F) {
            compute_graphic(c, made);
            g = made;
        }
    } else {
        g = ros_vdu_ttx_font[c - 32];
    }
    if ((w & MB_CONCEAL) && (f & TF_CONCEAL))
        g = ros_vdu_ttx_font[0];

    uint16_t rows[20];
    uint32_t h = w & (MB_DOUBLE | MB_BOTTOM);
    int from = w & MB_BOTTOM ? 10 : 0;
    for (int r = 0; r < 20; r++)
        rows[r] = h == MB_BOTTOM ? 0            /* under a double row: hidden */
                  : w & MB_DOUBLE ? g[from + r / 2] : g[r];
    uint8_t *p = cell(col, row);
    uint32_t ll = vdu.mv[MV_LINELENGTH], half = vdu.mv[MV_SCREENSIZE] / 2;
    for (int r = 0; r < 20; r++) {
        put16(p + r * ll, rows[r]);
        put16(p + half + r * ll, w & MB_FLASH ? 0 : rows[r]);
    }
}

/* ---- the control codes --------------------------------------------------------- */

/* DoPreControl: what a code does at its own cell */
static uint32_t pre_control(uint32_t w, uint32_t k, uint32_t *doubles)
{
    switch (k) {
    case TTX_STEADY:
        return w & ~MB_FLASH;
    case TTX_END_BOX:                           /* DoPreEndBox */
        return w & MB_PENDING_END ? w & ~MB_BOXED : w;
    case TTX_START_BOX:                         /* DoPreStartBox */
        return w & MB_PENDING_START ? w | MB_BOXED : w;
    case TTX_NORMAL_HEIGHT:                     /* DoSingleHeight */
        if (w & MB_DOUBLE)
            w &= ~MB_HELD_MASK;
        return w & ~MB_DOUBLE;
    case TTX_DOUBLE_HEIGHT:                     /* DoDoubleHeight */
        if (!(w & MB_DOUBLE))
            w &= ~MB_HELD_MASK;
        ++*doubles;
        return w | MB_DOUBLE;
    case TTX_CONCEAL:
        return w | MB_CONCEAL;
    case TTX_BLACK_BACKGD:
        return w & ~MB_BACK_MASK;
    case TTX_NEW_BACKGD:                        /* DoNewBackgd */
        return (w & ~MB_BACK_MASK) | (w & MB_FORE_MASK) << (MB_BACK_SHIFT - MB_FORE_SHIFT);
    case TTX_HOLD_GRAPH:
        return w | MB_HOLD;
    default:
        return w;
    }
}

/* DoPostControl: what it does from the next cell on */
static uint32_t post_control(uint32_t w, uint32_t k)
{
    if (k <= 0x07) {                            /* DoAlphaBlack, DoAlphaColour */
        if (k == 0 && !(ttx.flags & TF_BLACK_ENABLE))
            return w;
        w &= ~(MB_HELD_MASK | MB_FORE_MASK | MB_CONCEAL);
        return (w | k << MB_FORE_SHIFT) & ~MB_GRAPH;
    }
    if (k >= 0x10 && k <= 0x17) {               /* DoGraphBlack, DoGraphColour */
        if (k == 0x10 && !(ttx.flags & TF_BLACK_ENABLE))
            return w;
        w &= ~(MB_FORE_MASK | MB_CONCEAL);
        return w | (k & 7) << MB_FORE_SHIFT | MB_GRAPH;
    }
    switch (k) {
    case TTX_FLASH:
        return w | MB_FLASH;
    case TTX_END_BOX:                           /* DoPostEndBox */
        return w | MB_PENDING_END;
    case TTX_START_BOX:                         /* DoPostStartBox */
        return w | MB_PENDING_START;
    case TTX_CONTIGUOUS:
        return w & ~MB_SEPARATED;
    case TTX_SEPARATED:
        return w | MB_SEPARATED;
    case TTX_REL_GRAPH:
        return w & ~MB_HOLD;
    default:
        return w;
    }
}

/* UpdateHeldBits. A control code clears the held graphic unless hold is
 * on. A mosaic becomes the held graphic. */
static uint32_t update_held(uint32_t w)
{
    if (!(w & (0x60 | MB_HOLD)))
        return w & ~MB_HELD_MASK;
    if (!(w & MB_GRAPH) || !(w & 0x20))
        return w;
    w = (w & ~MB_HELD_MASK) | w << MB_HELD_SHIFT;
    return w & MB_SEPARATED ? w | MB_HELD_SEPARATED : w & ~MB_HELD_SEPARATED;
}

/* ---- the scan ------------------------------------------------------------------ */

/* TTXScanZap. This puts c into column x of row y and paints it. It goes on
 * along the row while what follows changes, and to the end of the line if
 * need be. Then it goes down while the next row's "bottom half" changes.
 * Rows above ymax also go on to the column after xmax (map indices, as the
 * kernel's addresses). When zapping, columns strictly between xmin and xmax
 * become spaces. */
static void scan_zap(uint32_t c, int32_t x, int32_t y, int32_t xmin, int32_t xmax, int32_t ymax,
                     int zap)
{
    int32_t eol = entry((int32_t)cols() - 1, y);        /* R7: the row's last word */
    int32_t at = y * (int32_t)ttx.stride + x;           /* R8: the word before x's */
    uint32_t prev = ttx.map[at];
    for (;;) {
        uint32_t n = ttx.dbl[y];                        /* R9 */
        for (;;) {
            uint32_t w = (prev & ~0xFFu) | c, k = w & 0x7F;
            if (k < 0x20)
                w = pre_control(w, k, &n);
            w = update_held(w);
            if (!(ttx.flags & TF_SUSPEND))
                paint(w, x, y);
            w &= ~(MB_PENDING_START | MB_PENDING_END);
            if (k < 0x20)
                w = post_control(w, k);
            uint32_t old = ttx.map[++at];
            ttx.map[at] = w;
            if ((old & 0x7F) == TTX_DOUBLE_HEIGHT)
                n--;
            uint32_t differ = (old ^ w) & ~0xFFu;
            prev = w;
            if (y < ymax && at <= xmax)
                differ = 1;
            if (y < ymax && at < xmax && xmin < at && zap)
                c = 32;
            else
                c = ttx.map[at + 1] & 0xFF;
            if (!differ || at == eol)
                break;
            x++;
        }
        ttx.dbl[y] = (uint8_t)n;
        if ((uint32_t)y == vdu.mv[MV_SCRBROW])
            return;
        y++;
        /* The next row is a bottom half if this one has double-height
         * codes and is a top half */
        uint32_t want = n ? (prev ^ MB_BOTTOM) & MB_BOTTOM : 0;
        uint32_t d = ttx.map[++eol];
        uint32_t change = (d ^ want) & MB_BOTTOM;
        d ^= change;
        ttx.map[eol] = d;
        if (!change && y >= ymax)
            return;
        at = eol;
        eol += (int32_t)cols();
        xmin += (int32_t)ttx.stride, xmax += (int32_t)ttx.stride;
        x = 0;
        prev = d;
        if (y < ymax && at < xmax && xmin < at && zap)
            c = 32;
        else
            c = ttx.map[at + 1] & 0xFF;
    }
}

/* TTXScanZap2: rescan columns l..r of rows t..b from the map */
static void rescan(int32_t l, int32_t t, int32_t r, int32_t b)
{
    scan_zap(ttx.map[entry(l, t)] & 0xFF, l, t, entry(l - 2, t), entry(r, t), b + 1, 0);
}

/* CountDoubles. This counts the double-height codes on rows t to b. */
static void count_doubles(int32_t t, int32_t b)
{
    for (int32_t y = t; y <= b; y++) {
        uint32_t n = 0;
        for (int32_t x = 0; x < (int32_t)cols(); x++)
            if ((ttx.map[entry(x, y)] & 0x7F) == TTX_DOUBLE_HEIGHT)
                n++;
        ttx.dbl[y] = (uint8_t)n;
    }
}

/* RefreshBitmap. This redraws the screen from the map, as it stands. */
static void refresh(void)
{
    uint32_t spare = 0;
    for (int32_t y = 0; y < (int32_t)ttx.rows; y++)
        for (int32_t x = 0; x < (int32_t)cols(); x++) {
            uint32_t w = (ttx.map[entry(x - 1, y)] & ~0xFFu) | (ttx.map[entry(x, y)] & 0xFF);
            uint32_t k = w & 0x7F;
            if (k < 0x20)
                w = pre_control(w, k, &spare);
            paint(update_held(w), x, y);
        }
}

/* ---- the VDU's teletext paths --------------------------------------------------- */

/* TTXDoChar. This puts a character at the cursor, which stays where it
 * is. */
void vdu_ttx_wrch(uint32_t c)
{
    if (!live())
        return;
    if (c == '#')                               /* the three old favourites */
        c = '_';
    else if (c == '_')
        c = '`';
    else if (c == '`')
        c = '#';
    scan_zap(c, vdu.cx, vdu.cy, 0, 0, 0, 0);
}

/* TTXFastCLS and FastCLS's colours. The map is set to its defaults, and
 * the page is cleared to black (transparent). */
void vdu_ttx_cls(void)
{
    if (!live())
        return;
    for (uint32_t i = 0; i < ttx.stride * ttx.rows; i++)
        ttx.map[i] = MB_DEFAULT;
    memset(ttx.dbl, 0, ttx.rows);
    update_colours(7, 8);
}

/* TTXClearBox. This sets columns l to r of rows t to b to spaces, and
 * updates what they change. */
void vdu_ttx_clear_box(int32_t l, int32_t t, int32_t r, int32_t b)
{
    if (!live())
        return;
    scan_zap(32, l, t, entry(l - 2, t), entry(r, t), b + 1, 1);
}

/* TTXSoftScrollUp and TTXSoftScrollDown. They move the text window's cells
 * by a row, leave spaces in the row left behind, and then rescan the
 * window. */
void vdu_ttx_scroll(int up)
{
    if (!live())
        return;
    int32_t l = vdu.twl, t = vdu.twt, r = vdu.twr, b = vdu.twb;
    size_t n = (size_t)(r - l + 1) * sizeof *ttx.map;
    if (up)
        for (int32_t y = t; y < b; y++)
            memmove(&ttx.map[entry(l, y)], &ttx.map[entry(l, y + 1)], n);
    else
        for (int32_t y = b; y > t; y--)
            memmove(&ttx.map[entry(l, y)], &ttx.map[entry(l, y - 1)], n);
    int32_t clear = up ? b : t;
    for (int32_t x = l; x <= r; x++)
        ttx.map[entry(x, clear)] = (ttx.map[entry(x, clear)] & ~0xFFu) | 32;
    count_doubles(t, b);
    rescan(l, t, r, b);
}

/* TTXScrollLeft and TTXScrollRight. They move the window's (or the
 * screen's) cells by a column across, leave spaces in the column left
 * behind, and then rescan. */
void vdu_ttx_scroll_side(int left, int screen)
{
    if (!live())
        return;
    int32_t l = vdu.twl, t = vdu.twt, r = vdu.twr, b = vdu.twb;
    if (screen)
        l = 0, t = 0, r = (int32_t)vdu.mv[MV_SCRRCOL], b = (int32_t)vdu.mv[MV_SCRBROW];
    int32_t out = left ? l : r;                 /* TTXSideScroll2 */
    for (int32_t y = t; y <= b; y++)
        if ((ttx.map[entry(out, y)] & 0x7F) == TTX_DOUBLE_HEIGHT)
            ttx.dbl[y]--;
    size_t n = (size_t)(r - l) * sizeof *ttx.map;
    for (int32_t y = t; y <= b; y++)
        if (left)
            memmove(&ttx.map[entry(l, y)], &ttx.map[entry(l + 1, y)], n);
        else
            memmove(&ttx.map[entry(l + 1, y)], &ttx.map[entry(l, y)], n);
    int32_t in = left ? r : l;                  /* TTXSideScroll3 */
    for (int32_t y = t; y <= b; y++)
        ttx.map[entry(in, y)] = (ttx.map[entry(in, y)] & ~0xFFu) | 32;
    rescan(l, t, r, b);
}

/* TTXReadCharacter. This gives OS_Byte 135's character, from the map. */
uint32_t vdu_ttx_read_char(void)
{
    if (!live())
        return 0;
    uint32_t c = ttx.map[entry(vdu.cx, vdu.cy)] & 0xFF;
    return c == '#' ? '`' : c == '`' ? '_' : c == '_' ? '#' : c;
}

/* ---- VDU 23,18 --------------------------------------------------------------------- */

/* SetTTXPalette. This sets the sixteen colours in both flash states, then
 * the border, through PaletteV. */
static void set_palette(const uint32_t pal[16], uint32_t border)
{
    if (vdu.mv[MV_LOG2BPP] <= 3) {
        vdu_palette_bulk(pal, 16, 17);
        vdu_palette_bulk(pal, 16, 18);
    }
    vdu_palette_set(0, 24, border);
}

/* TTXPalette_Solid reads on into TTXPalette_Mixed, as the kernel's sixteen
 * words do. TTXPalette_Mixed has 8-15 transparent. */
static const uint32_t pal_solid[16] = {
    0x00000000u, 0x0000FF00u, 0x00FF0000u, 0x00FFFF00u,
    0xFF000000u, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFF00u,
    0x00000000u, 0x0000FF00u, 0x00FF0000u, 0x00FFFF00u,
    0xFF000000u, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFF00u,
};
static const uint32_t pal_mixed[16] = {
    0x00000000u, 0x0000FF00u, 0x00FF0000u, 0x00FFFF00u,
    0xFF000000u, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFF00u,
    0x000000FFu, 0x0000FFFFu, 0x00FF00FFu, 0x00FFFFFFu,
    0xFF0000FFu, 0xFF00FFFFu, 0xFFFF00FFu, 0xFFFFFFFFu,
};

/* Vdu23_18_0 sets the transparency mode: 0 is opaque, 1 is mix, 2 is box
 * and 3 is TV. */
static void transparency(uint32_t mode)
{
    mode &= 3;
    uint32_t f = ttx.flags;
    if ((f & TF_TRANS_MASK) == mode << TF_TRANS_SHIFT)
        return;
    if (mode == 0)
        set_palette(pal_solid, 0);
    f |= TF_FG_TRANS_EOR | TF_BG_TRANS_EOR | TF_FG_TRANS_BIC | TF_BG_TRANS_BIC;
    if (mode != 3) {
        f &= mode != 2 ? ~TF_FG_TRANS_EOR : ~TF_FG_TRANS_BIC;
        f &= mode != 0 ? ~TF_BG_TRANS_BIC : ~TF_BG_TRANS_EOR;
    }
    uint32_t was = f;                           /* with the old mode's bits */
    ttx.flags = (f & ~TF_TRANS_MASK) | mode << TF_TRANS_SHIFT;
    if (vdu.mv[MV_LOG2BPP] <= 3)
        refresh();
    if (!(was & TF_TRANS_MASK))
        set_palette(pal_mixed, 0xFF);
    if (vdu.mv[MV_LOG2BPP] > 3) {
        vdu.tfore = 0xFFFFFFFFu;                /* the next paint sets the colours */
        refresh();
    }
}

/* Vdu23_18. The value n is QQ+1 and p is QQ+2. It returns 0 if n is not one
 * of these (UnknownVdu23). */
int vdu_ttx_vdu23_18(uint32_t n, uint32_t p)
{
    if (n >= 4)
        return 0;
    if (!live())
        return 1;
    uint32_t old = ttx.flags;
    switch (n) {
    case 0:
        transparency(p);
        break;
    case 1:                                     /* suspend, and resume with a refresh */
        ttx.flags = p & 1 ? old | TF_SUSPEND : old & ~TF_SUSPEND;
        if (old & ~ttx.flags)
            refresh();
        break;
    case 2:                                     /* conceal (0) or reveal (1) */
        ttx.flags = p & 1 ? old & ~TF_CONCEAL : old | TF_CONCEAL;
        if (!(ttx.flags & TF_SUSPEND))
            refresh();
        break;
    case 3: {                                   /* black foregrounds, &80 and &90 */
        uint32_t want = p & 1 ? old | TF_BLACK_ENABLE : old & ~TF_BLACK_ENABLE;
        if (!((old ^ want) & TF_BLACK_ENABLE))
            break;
        ttx.flags = want | TF_SUSPEND;          /* the map again, without painting */
        rescan(0, 0, (int32_t)vdu.mv[MV_SCRRCOL], (int32_t)vdu.mv[MV_SCRBROW]);
        ttx.flags = want;
        if (!(want & TF_SUSPEND))
            refresh();
        break;
    }
    }
    return 1;
}

/* ---- the VSync ------------------------------------------------------------------------ */

/* TeletextFlashTest. The other bank is shown when the count runs out. */
void vdu_ttx_vsync(void)
{
    if (!live() || --ttx.count)
        return;
    ttx.offset ^= vdu.mv[MV_SCREENSIZE] / 2;
    ttx.count = ttx.offset ? OFF_FLASH_TIME : ON_FLASH_TIME;
    uint32_t r[4] = { 0, vdu.display_start + ttx.offset };      /* VInit: in the bank shown */
    vdu_graphicsv(GV_SET_DMA_ADDRESS, r);
}
