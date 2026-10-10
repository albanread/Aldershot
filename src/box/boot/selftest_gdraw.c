/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_gdraw.c: GDraw, the anti-aliasing Draw
 * (modules/gdraw), against the vectors measured on RISC OS 5.30 with
 * GDraw 3.12, and its own contract where nothing is measured yet.
 *
 * As the measurements were taken: a 32 x 32 sprite, new format at 90 dpi
 * (one pixel is 2 OS units, so pixel p is p x 512 in path units), cleared
 * by writing its memory, output switched to it by OS_SpriteOp 60, the
 * colour set by ColourTrans_SetGCOL, the identity matrix, flatness 0.
 * Rows are counted from the top of the sprite (memory order): the row for
 * pixel y is 31 - y.  Pixels are printed from x = 6 to 24.
 *
 * Numbered checks are the measured vectors (#1-#37), each to the
 * pixel as measured.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check

#define GDRAW 0x44540u
#define DRAW 0x40700u

static const char *last_err;

/* A SWI: its registers in and out.  Gives 0, or the error number */
static uint32_t swi(uint32_t number, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 10 * sizeof r[0]);
    last_err = s.v ? ((os_error *)ros_ptr(s.r[0]))->errmess : "";
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static uint32_t call(uint32_t n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4,
                     uint32_t r5, uint32_t r6, uint32_t r7, uint32_t *out0)
{
    uint32_t r[10] = { r0, r1, r2, r3, r4, r5, r6, r7 };
    uint32_t e = swi(n, r);
    if (out0)
        *out0 = r[0];
    return e;
}

/* ---- memory: one RMA block, carved up ---------------------------------------------------- */

static uint8_t *mem;
static uint32_t base;
enum {
    O_AREA = 0, AREA_SIZE = 8192,           /* the sprite area */
    O_PATHR = 8192, O_PATHT = 8448, O_LINE = 8704, O_PATHB = 8960,
    O_MATRIX = 9216, O_CAPS = 9280, O_MAT2 = 9344, O_PATHQ = 9472, O_PATHH = 9728,
    O_BUF1 = 10240, O_BUF2 = 14336, BUF_SIZE = 4096,
    O_REGION = 18432, REGION_SIZE = 8192,
    MEM_SIZE = 32768,
};

static uint32_t at(uint32_t off) { return base + off; }

static uint32_t *path_put(uint32_t off, const int32_t *w, unsigned n)
{
    uint32_t *p = (uint32_t *)(mem + off);
    for (unsigned k = 0; k < n; k++)
        p[k] = (uint32_t)w[k];
    return p;
}

#define P(v) ((int32_t)((v) * 512))

static void make_paths(void)
{
    /* Rectangle R: (10.1, 5.5)-(20.75, 25.25).  A BASIC probe truncates
     * 10.1 x 512 to 5171 */
    const int32_t r[] = { 2, 5171, P(5.5), 8, P(20.75), P(5.5), 8, P(20.75), P(25.25),
                          8, 5171, P(25.25), 5, 0 };
    path_put(O_PATHR, r, sizeof r / sizeof r[0]);
    /* Triangle T: (3, 4), (28, 11), (9, 29) */
    const int32_t t[] = { 2, P(3), P(4), 8, P(28), P(11), 8, P(9), P(29), 5, 0 };
    path_put(O_PATHT, t, sizeof t / sizeof t[0]);
    /* The line (4, 15.5)-(28, 15.5) */
    const int32_t l[] = { 2, P(4), P(15.5), 8, P(28), P(15.5), 0 };
    path_put(O_LINE, l, sizeof l / sizeof l[0]);
    /* Q: (10.25, 5.75)-(20.75, 25.25).  H: (10.5, 6)-(20.5, 25) */
    const int32_t q[] = { 2, P(10.25), P(5.75), 8, P(20.75), P(5.75), 8, P(20.75), P(25.25),
                          8, P(10.25), P(25.25), 5, 0 };
    path_put(O_PATHQ, q, sizeof q / sizeof q[0]);
    const int32_t h[] = { 2, P(10.5), P(6), 8, P(20.5), P(6), 8, P(20.5), P(25), 8, P(10.5), P(25), 5, 0 };
    path_put(O_PATHH, h, sizeof h / sizeof h[0]);
    /* Pixels (2, 2)-(30, 30) */
    const int32_t b[] = { 2, P(2), P(2), 8, P(30), P(2), 8, P(30), P(30), 8, P(2), P(30), 5, 0 };
    path_put(O_PATHB, b, sizeof b / sizeof b[0]);
    const int32_t id[] = { 65536, 0, 0, 65536, 0, 0 };
    path_put(O_MATRIX, id, 6);
    const int32_t rot[] = { 46341, 46341, -46341, 46341, 1000, 2000 };
    path_put(O_MAT2, rot, 6);
    /* joins mitred, caps butt, mitre limit 10 */
    const int32_t caps[] = { 0, 10 << 16, 0, 0 };
    path_put(O_CAPS, caps, 4);
}

/* ---- the sprite ---------------------------------------------------------------------------- */

static uint32_t l2bpp, image;           /* the sprite's depth and pixels */
static uint32_t saved[4];

/* A 32 x 32 sprite of 1 << l2 bits a pixel (0-5).  Its type is 1-6, and it has no palette */
static uint32_t make_sprite(uint32_t l2)
{
    static const uint32_t type[6] = { 1, 2, 3, 4, 5, 6 };  /* 1, 2, 4, 8, 16, 32 bpp */
    uint32_t a = at(O_AREA), sp = a + 16, words = (32u << l2) / 32;
    uint32_t img = 44, size = img + words * 4 * 32;
    memset(mem + O_AREA, 0, AREA_SIZE);
    ros_st32(a, AREA_SIZE), ros_st32(a + 4, 1), ros_st32(a + 8, 16), ros_st32(a + 12, 16 + size);
    ros_st32(sp, size);
    memcpy(ros_ptr(sp + 4), "gdtest", 6);
    ros_st32(sp + 16, words - 1), ros_st32(sp + 20, 31);
    ros_st32(sp + 24, 0), ros_st32(sp + 28, 31);
    ros_st32(sp + 32, img), ros_st32(sp + 36, img);
    ros_st32(sp + 40, type[l2] << 27 | 90u << 14 | 90u << 1 | 1);
    l2bpp = l2, image = sp + img;
    return sp;
}

/* Every pixel white: &FF bytes, but a 32 bpp pixel's top byte 0 */
static void clear(void)
{
    uint8_t *p = ros_ptr(image);
    memset(p, 0xFF, (32u << l2bpp) / 8 * 32);
    if (l2bpp == 5)
        for (int i = 0; i < 32 * 32; i++)
            p[4 * i + 3] = 0;
}

static uint32_t pix(int x, int y)
{
    uint32_t row = image + (uint32_t)(31 - y) * ((32u << l2bpp) / 8);
    switch (l2bpp) {
    case 5: return ros_ld32(row + 4 * (uint32_t)x);
    case 4: return ros_ld32(row + 2 * (uint32_t)x) & 0xFFFF;
    case 3: return ros_ld8(row + (uint32_t)x);
    default: {
        uint32_t bpp = 1u << l2bpp, bit = (uint32_t)x * bpp;
        return (ros_ld8(row + bit / 8) >> (bit & 7)) & ((1u << bpp) - 1);
    }
    }
}

static void to_sprite(uint32_t sp)
{
    uint32_t r[10] = { 512 + 60, at(O_AREA), sp, 0 };
    swi(OS_SpriteOp, r);
    memcpy(saved, r, sizeof saved);
}

static void to_screen(void)
{
    uint32_t r[10] = { saved[0], saved[1], saved[2], saved[3] };
    swi(OS_SpriteOp, r);
}

/* ColourTrans_SetGCOL: a palette word &BBGGRR00, foreground, an action */
static void colour(uint32_t rgb, uint32_t action)
{
    uint32_t r[10] = { rgb, 0, 0, 0, action };
    swi(ColourTrans_SetGCOL, r);
}

#define RED 0x0000FF00u
#define BLACK 0u
#define BLUE 0xFF000000u

static uint32_t fill(uint32_t n, uint32_t path, uint32_t style)
{
    return call(n, at(path), style, at(O_MATRIX), 0, 0, 0, 0, 0, NULL);
}

/* The line (4, y)-(28, y), y in pixels */
static void set_line(double y)
{
    const int32_t l[] = { 2, P(4), P(y), 8, P(28), P(y), 0 };
    path_put(O_LINE, l, sizeof l / sizeof l[0]);
}

static uint32_t stroke(uint32_t style, uint32_t width)
{
    return call(GDRAW + 4, at(O_LINE), style, at(O_MATRIX), 0, width, at(O_CAPS), 0, 0, NULL);
}

/* Row y's pixels x 6-24, as text.  Also the same compared */
static char text[512];

static const char *row_text(int y)
{
    char *p = text;
    for (int x = 6; x <= 24; x++)
        p += snprintf(p, 16, "%X ", pix(x, y));
    return text;
}

static const char *col_text(int x, int y0, int y1)
{
    char *p = text;
    for (int y = y1; y >= y0; y--)
        p += snprintf(p, 16, "%X ", pix(x, y));
    return text;
}

static int row_is(int y, const uint32_t want[19])
{
    for (int x = 6; x <= 24; x++)
        if (pix(x, y) != want[x - 6])
            return 0;
    return 1;
}

/* want[] for a row: v inside [a, b], else bg; at x = c, cv (c < 0: none) */
static void row_want(uint32_t want[19], uint32_t bg, int a, int b, uint32_t v, int c, uint32_t cv)
{
    for (int x = 6; x <= 24; x++)
        want[x - 6] = x >= a && x <= b ? v : bg;
    if (c >= 0)
        want[c - 6] = cv;
}

/* A run of grey levels as the measurements are written, "FF 10 00x8 ..."
 * (x for the multiplication sign): each byte v as the pixel &vvvvvv */
static int greys(const char *spec, uint32_t *out, int max)
{
    int n = 0;
    while (*spec) {
        char *end;
        unsigned long v = strtoul(spec, &end, 16);
        if (end == spec)
            break;
        unsigned long times = 1;
        if (*end == 'x')
            times = strtoul(end + 1, &end, 10);
        for (unsigned long k = 0; k < times && n < max; k++)
            out[n++] = (uint32_t)v * 0x010101u;
        while (*end == ' ')
            end++;
        spec = end;
    }
    return n;
}

/* Row r (from the top) at x 6-24, or column x at rows 2-28 (from the top),
 * against a run of greys */
static int row_greys(int r, const char *spec)
{
    uint32_t want[32];
    if (greys(spec, want, 32) != 19)
        return 0;
    for (int x = 6; x <= 24; x++)
        if (pix(x, 31 - r) != want[x - 6])
            return 0;
    return 1;
}

static int col_greys(int x, const char *spec)
{
    uint32_t want[32];
    if (greys(spec, want, 32) != 27)
        return 0;
    for (int r = 2; r <= 28; r++)
        if (pix(x, 31 - r) != want[r - 2])
            return 0;
    return 1;
}

/* The rectangle's row 15 (y = 16) after a fill of the given style */
static int rect_row(uint32_t style, uint32_t bg, uint32_t in, int full_to, int part, uint32_t pv)
{
    clear();
    uint32_t e = fill(GDRAW + 2, O_PATHR, style);
    uint32_t want[19];
    row_want(want, bg, 10, full_to, in, part, pv);
    return !e && row_is(16, want);
}

/* ---- the vectors ------------------------------------------------------------------------------ */

static void fresh(void)
{
    uint32_t r0 = 1;
    uint32_t e = call(GDRAW + 24, 0, 0, 0, 0, 0, 0, 0, 0, &r0);
    check(!e && r0 == 0, "GDraw #28: GetClipRegion, fresh -- 0", "&%X, error &%X", r0, e);
    r0 = 1;
    e = call(GDRAW + 61, 0, 0, 0, 0, 0, 0, 0, 0, &r0);
    check(!e && r0 == 0, "GDraw #29: reason 61 (SWI &4457D), fresh -- 0: DrawV not claimed",
          "&%X, error &%X", r0, e);
}

static void errors(void)
{
    uint32_t e = fill(GDRAW + 3, O_PATHR, 0x30);
    check(e == 0x9FF && !strcmp(last_err, "Facility not in this version of Draw"),
          "GDraw #19: FillFP -- &9FF \"Facility not in this version of Draw\"", "&%X %s", e, last_err);
    static const uint32_t fp[] = { 1, 5, 7, 9, 11, 13, 15, 17 };
    int ok = 1;
    for (unsigned k = 0; k < sizeof fp / sizeof fp[0]; k++)
        ok &= fill(GDRAW + fp[k], O_PATHR, 0x30) == 0x9FF;
    check(ok, "GDraw: every ...FP SWI, and ProcessPathFP -- &9FF", NULL);
    e = fill(GDRAW + 2, O_PATHR, 0x130);
    check(e == 0x982 && !strcmp(last_err, "Reserved bits not zero"),
          "GDraw #20: Fill, style &130 -- &982 \"Reserved bits not zero\"", "&%X %s", e, last_err);
    ok = 1;
    static const uint32_t wind[] = { 0xB1, 0xB3 };
    for (unsigned k = 0; k < 2; k++) {
        e = fill(GDRAW + 2, O_PATHR, wind[k]);
        ok &= e == 0x9FF && !strcmp(last_err, "Winding rule not available in this version of Draw");
    }
    check(ok, "GDraw #21: Fill, style &B1 and &B3 -- &9FF \"Winding rule not available...\"",
          "&%X %s", e, last_err);
    ok = 1;
    static const uint32_t plot[] = { 0x80, 0x90, 0xA0, 0xB4, 0xBC };
    for (unsigned k = 0; k < 5; k++) {
        e = fill(GDRAW + 2, O_PATHR, plot[k]);
        ok &= e == 0x9FF && !strcmp(last_err, "Plot rule not available in this version of Draw");
    }
    check(ok, "GDraw #22: Fill, style &80, &90, &A0, &B4, &BC -- &9FF \"Plot rule not available...\"",
          "&%X %s", e, last_err);
    e = stroke(0x80, 0);
    check(e == 0x9FF && !strcmp(last_err, "Plot rule not available in this version of Draw"),
          "GDraw #18: a thin stroke, style &80 -- &9FF \"Plot rule not available...\"", "&%X %s", e,
          last_err);
    e = call(GDRAW + 16, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    check(e == 0x9FF, "GDraw: ClipPathToPath, its registers unknown -- refused, &9FF", "&%X", e);
    e = call(GDRAW + 25, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    uint32_t e2 = call(GDRAW + 60, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    check(e == 0x1E6 && e2 == 0x1E6, "GDraw: SWIs 25 and 60 -- &1E6", "&%X &%X %s", e, e2, last_err);
}

static void depth32(void)
{
    uint32_t sp = make_sprite(5);
    to_sprite(sp);
    colour(RED, 0);
    uint32_t want[19];

    check(rect_row(0x30, 0xFFFFFF, 0xFF, 20, -1, 0), "GDraw #1: Fill R, &30, 32 bpp -- row 15, x 10-20 red",
          "%s", row_text(16));
    check(rect_row(0xB0, 0xFFFFFF, 0xFF, 19, 20, 0x4040FF),
          "GDraw #2: Fill R, &B0 -- row 15, x 10-19 red, x 20 &4040FF (12/16)", "%s", row_text(16));
    int ok = 1;
    for (int y = 2; y <= 28 && ok; y++) {
        int row = 31 - y;
        uint32_t w = row == 6 ? 0xBFBFFF : row == 26 ? 0x8080FF : row >= 7 && row <= 25 ? 0xFF : 0xFFFFFF;
        ok &= pix(15, y) == w;
    }
    check(ok, "GDraw #3: Fill R, &B0 -- column 15: row 6 &BFBFFF, rows 7-25 red, row 26 &8080FF",
          "%s", col_text(15, 3, 29));
    check(rect_row(0xB8, 0xFFFFFF, 0xFF, 20, -1, 0),
          "GDraw #4: Fill R, &B8 -- row 15, x 10-20 red (x 20 full)", "%s", row_text(16));
    check(rect_row(0xB2, 0xFFFFFF, 0xFF, 19, 20, 0x4040FF), "GDraw #5: Fill R, &B2 -- as #2", "%s",
          row_text(16));
    check(rect_row(0xF0, 0xFFFFFF, 0xFF, 19, 20, 0x4040FF), "GDraw #6: Fill R, &F0 -- as #2 (bit 6 ignored)",
          "%s", row_text(16));

    colour(BLACK, 0);
    static const uint32_t t7[19] = { 0xBFBFBF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x101010, 0xCFCFCF,
                                     0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF };
    clear();
    uint32_t e = fill(GDRAW + 2, O_PATHT, 0xB0);
    check(!e && row_is(19, t7),
          "GDraw #7: Fill T, &B0, black -- row 12: &BFBFBF, 11 x 0, &101010, &CFCFCF, 5 x &FFFFFF",
          "%s", row_text(19));
    clear();
    e = fill(GDRAW + 2, O_PATHT, 0xB8);
    int rest = pix(6, 19) == 0x9F9F9F;
    for (int x = 7; x <= 24; x++)
        rest &= pix(x, 19) == t7[x - 6];
    check(!e && rest, "GDraw #8: Fill T, &B8 -- row 12 as #7, but the first pixel &9F9F9F", "%s",
          row_text(19));
    clear();
    e = fill(GDRAW + 2, O_PATHT, 0x38);
    row_want(want, 0xFFFFFF, 6, 19, 0, -1, 0);
    check(!e && row_is(19, want), "GDraw #9: Fill T, &38 -- row 12, x 6-19 black", "%s",
          row_text(19));

    /* #30-#35, #37: black on white */
    clear();
    e = fill(GDRAW + 2, O_PATHQ, 0xB8);
    check(!e && row_greys(15, "FFx4 40 00x10 FFx4") && col_greys(15, "FFx4 BF 00x19 80 FFx2"),
          "GDraw #30: Fill Q, &B8 -- row 15 and column 15", "row %s", row_text(16));
    clear();
    e = fill(GDRAW + 2, O_PATHH, 0xB8);
    check(!e && row_greys(15, "FFx4 80 00x9 40 FFx4") && col_greys(15, "FFx5 00x19 BF FFx2"),
          "GDraw #31: Fill H, &B8 -- row 15 and column 15", "row %s", row_text(16));
    static const char *const t32[5] = {
        "FF 10 00x8 10 CF FFx7", "CF 00x10 10 CF FFx6", "9F 00x11 10 CF FFx5",
        "60 00x12 10 CF FFx4", "20 00x13 10 9F FFx3" };
    static const char *const t33[5] = {
        "FF 30 00x8 30 EF FFx7", "EF 00x10 30 EF FFx6", "BF 00x11 10 CF FFx5",
        "80 00x12 10 CF FFx4", "40 00x13 10 CF FFx3" };
    for (int pass = 0; pass < 2; pass++) {
        clear();
        e = fill(GDRAW + 2, O_PATHT, pass ? 0xB0 : 0xB8);
        int okt = !e, bad = -1;
        for (int r = 10; r <= 14; r++)
            if (!row_greys(r, (pass ? t33 : t32)[r - 10]) && bad < 0)
                bad = r, okt = 0;
        check(okt, pass ? "GDraw #33: Fill T, &B0 -- rows 10-14" : "GDraw #32: Fill T, &B8 -- rows 10-14",
              "row %d: %s", bad, bad >= 0 ? row_text(31 - bad) : "");
    }
    clear();
    e = fill(GDRAW + 2, O_PATHH, 0x38);
    check(!e && col_greys(15, "FFx5 00x20 FFx2"),
          "GDraw #34: Fill H, &38 (no AA) -- column 15, rows 7-26 black", "%s", col_text(15, 3, 29));
    clear();
    fill(DRAW + 2, O_PATHH, 0x38);
    check(col_greys(15, "FFx4 00x20 FFx3"), "GDraw #34: Draw_Fill H, &38 -- rows 6-25: GDraw's differs",
          "%s", col_text(15, 3, 29));
    clear();
    e = fill(GDRAW + 2, O_PATHT, 0x38);
    static const int t35[5][2] = { { 7, 16 }, { 6, 17 }, { 6, 19 }, { 6, 20 }, { 6, 21 } };
    ok = !e;
    for (int r = 10; r <= 14; r++) {
        row_want(want, 0xFFFFFF, t35[r - 10][0], t35[r - 10][1], 0, -1, 0);
        ok &= row_is(31 - r, want);
    }
    check(ok, "GDraw #35: Fill T, &38 (no AA) -- rows 10-14 black to x 16, 17, 19, 20, 21", "row 11: %s",
          row_text(20));
    clear();
    fill(DRAW + 2, O_PATHT, 0x38);
    row_want(want, 0xFFFFFF, 6, 18, 0, -1, 0);
    check(row_is(20, want), "GDraw #35: Draw_Fill T, &38 -- row 11 to x 18: GDraw's differs", "%s",
          row_text(20));
    static const uint32_t p37[3] = { O_PATHH, O_PATHQ, O_PATHT };
    static uint32_t by_draw[32 * 32];
    ok = 1;
    for (int k = 0; k < 3; k++) {
        clear();
        ok &= !fill(DRAW + 2, p37[k], 0x30);
        for (int i = 0; i < 32 * 32; i++)
            by_draw[i] = pix(i & 31, i >> 5);
        clear();
        ok &= !fill(GDRAW + 2, p37[k], 0x30);
        for (int i = 0; i < 32 * 32; i++)
            ok &= pix(i & 31, i >> 5) == by_draw[i];
    }
    check(ok, "GDraw #37: Fill H, Q and T, &30 (no AA) -- identical to Draw_Fill's", NULL);

    /* #13: AND and invert anti-alias as overwrite */
    static const uint32_t acts[] = { 2, 4, 1, 3 };
    for (unsigned k = 0; k < 4; k++) {
        colour(RED, acts[k]);
        int okk = rect_row(0xB0, 0xFFFFFF, 0xFF, 19, 20, 0x4040FF);
        char what[128];
        snprintf(what, sizeof what, k < 2 ? "GDraw #13: Fill R, &B0, action %u -- as #2"
                                          : "GDraw: Fill R, &B0, action %u (unstable in GDraw) -- as #2, overwrite",
                 acts[k]);
        check(okk, what, "%s", row_text(16));
    }
    colour(RED, 3);
    clear();
    e = fill(GDRAW + 2, O_PATHR, 0x30);
    row_want(want, 0xFFFFFF, 10, 20, 0xFFFF00, -1, 0);
    check(!e && row_is(16, want), "GDraw: Fill R, &30, EOR -- Draw's EOR fill", "%s", row_text(16));

    /* strokes: (4, y)-(28, y); column 16's rows from the top as greys */
    colour(BLACK, 0);
    static const struct { const char *what; double y; uint32_t width, style; int top; const char *col; } st[] = {
        { "GDraw #14: Stroke y 15.5, 3 px, &80 -- rows 15-17 black", 15.5, 1536, 0x80, 15, "00x3" },
        { "GDraw #14: Stroke y 15.5, 3 px, &80000080 -- rows 15-17 black", 15.5, 1536, 0x80000080u, 15, "00x3" },
        { "GDraw #15: Stroke y 15.5, 3 px, &38 -- rows 15-18 black", 15.5, 1536, 0x38, 15, "00x4" },
        { "GDraw #16: Stroke y 15.5, 3 px, &B8 -- rows 15-17 black, row 18 &BFBFBF", 15.5, 1536, 0xB8, 15,
          "00x3 BF" },
        { "GDraw #16: Stroke y 15.5, 3 px, &800000B8 -- rows 15-17 black, row 18 &BFBFBF", 15.5, 1536,
          0x800000B8u, 15, "00x3 BF" },
        { "GDraw #36: Stroke y 15, 2 px, &B8 -- rows 16-17 black, 18 &BF", 15, 1024, 0xB8, 16, "00x2 BF" },
        { "GDraw #36: Stroke y 15, 2 px, &80 -- rows 16-17 black", 15, 1024, 0x80, 16, "00x2" },
        { "GDraw #36: Stroke y 15, 2 px, &38 -- rows 16-18 black", 15, 1024, 0x38, 16, "00x3" },
        { "GDraw #36: Stroke y 15.5, 2 px, &B8 -- 15 &80, 16 black, 17 &40", 15.5, 1024, 0xB8, 15, "80 00 40" },
        { "GDraw #36: Stroke y 15.5, 2 px, &80 -- 15 &80, 16 black, 17 &80", 15.5, 1024, 0x80, 15, "80 00 80" },
        { "GDraw #36: Stroke y 15.5, 2 px, &38 -- rows 15-17 black", 15.5, 1024, 0x38, 15, "00x3" },
        { "GDraw #36: Stroke y 15, 3 px, &B8 -- 15 &80, 16-17 black, 18 &40", 15, 1536, 0xB8, 15, "80 00x2 40" },
        { "GDraw #36: Stroke y 15, 3 px, &80 -- 15 &80, 16-17 black, 18 &80", 15, 1536, 0x80, 15, "80 00x2 80" },
        { "GDraw #36: Stroke y 15, 3 px, &38 -- rows 15-18 black", 15, 1536, 0x38, 15, "00x4" },
    };
    for (unsigned k = 0; k < sizeof st / sizeof st[0]; k++) {
        set_line(st[k].y);
        clear();
        e = stroke(st[k].style, st[k].width);
        uint32_t run[32];
        int nrun = greys(st[k].col, run, 32);
        ok = !e;
        for (int row = 0; row < 32; row++) {
            int i = row - st[k].top;
            ok &= pix(16, 31 - row) == (i >= 0 && i < nrun ? run[i] : 0xFFFFFFu);
        }
        check(ok, st[k].what, "error &%X; column 16, rows 0-31: %s", e, col_text(16, 0, 31));
    }
    set_line(15.5);
    static const uint32_t thin[] = { 0x18, 0 };
    for (unsigned k = 0; k < 2; k++) {
        clear();
        e = stroke(thin[k], 0);
        row_want(want, 0, 6, 24, 0, -1, 0);
        ok = !e && row_is(15, want);
        for (int row = 0; row < 32; row++)
            ok &= pix(16, 31 - row) == (row == 16 ? 0u : 0xFFFFFFu);
        check(ok, k ? "GDraw #17: a thin stroke, style 0 -- one black pixel line on row 16"
                    : "GDraw #17: a thin stroke, style &18 -- one black pixel line on row 16",
              "error &%X; row 16: %s", e, row_text(15));
    }

    /* #26, #27: SetFillStyle recorded, and no visible effect */
    int ok26 = 1, ok27 = 1;
    for (uint32_t t = 1; t <= 3; t++) {
        e = call(GDRAW + 20, t, at(O_MATRIX), 0x800, 0x2000, 0x3800, 0x2000, 0, 0, NULL);
        uint32_t blk = 0;
        uint32_t e2 = call(GDRAW + 23, 0, 0, 0, 0, 0, 0, 0, 0, &blk);
        const uint32_t w[6] = { t, at(O_MATRIX), 0x800, 0x2000, 0x3800, 0x2000 };
        for (uint32_t k = 0; !e && !e2 && blk && k < 6; k++)
            ok26 &= ros_ld32(blk + 4 * k) == w[k];
        ok26 &= !e && !e2 && blk;
        uint32_t r[10] = { BLUE, 0, 0, 0x80, 0 };
        swi(ColourTrans_SetGCOL, r);
        colour(RED, 0);
        clear();
        e = fill(GDRAW + 2, O_PATHB, 0xB0);
        for (int x = 3; x <= 29; x++)
            ok27 &= pix(x, 16) == 0xFF;
        for (int y = 3; y <= 29; y++)
            ok27 &= pix(16, y) == 0xFF;
        ok27 &= !e;
    }
    check(ok26, "GDraw #26: ReadFillStyle after SetFillStyle 1-3 -- words 0-5 are R0-R5", NULL);
    check(ok27, "GDraw #27: SetFillStyle 1-3, then a fill -- flat red: no visible effect", "%s",
          row_text(16));
    call(GDRAW + 20, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    e = call(GDRAW + 20, 4, at(O_MATRIX), 0, 0, 0, 0, 0, 0, NULL);
    uint32_t e2 = call(GDRAW + 20, 0x301, at(O_MATRIX), 0, 0, 0, 0, 0, 0, NULL);
    uint32_t e3 = call(GDRAW + 20, 0x10101, at(O_MATRIX), 0, 0, 0, 0, 0, 0, NULL);
    check(e && e2 && !e3, "GDraw: SetFillStyle -- type 4 and field 3 refused, bit 16 accepted",
          "&%X &%X &%X", e, e2, e3);
    /* and none left set: with bit 16, GDraw takes the next Stroke as a
     * graduated fill's axis (#137), and the tests after these stroke */
    call(GDRAW + 20, 0, 0, 0, 0, 0, 0, 0, 0, NULL);

    /* #137: graduated fills, as AWRender draws them. SetFillStyle type 2
     * with bit 16 and a table of COLORREFs, a Stroke of the axis (not
     * drawn), then the fill in the table's colours; measured against GDraw
     * 3.12 run by the ARM container.  Entry i is &00ii40ii (its index
     * readable back from the pixel).  Mode 0 (bits 8-9 = 0): 256 entries,
     * a step for each 256th of the axis (4, y)-(28, y), 3.12's row 16 to
     * the step.  Mode 1: 2048 entries, eight a step, with 7 - (y mod 8) the one.
     * The plot after the fill is flat: the style is used up. */
    static const uint8_t row16[32] = { 0, 0, 0, 0, 0, 10, 21, 32, 42, 53, 64, 74, 85, 96, 106, 117,
                                       128, 138, 149, 160, 170, 181, 192, 202, 213, 224, 234, 245,
                                       255, 255, 255, 255 };
    {
        uint32_t *tab = ros_rma_alloc(2048 * 4);
        for (uint32_t i = 0; i < 2048; i++)
            tab[i] = (i & 0xFF) | 0x4000u | (i >> 8) << 16;
        int okg = 1, okd = 1, okflat = 1;
        for (uint32_t mode = 0; mode < 2; mode++) {
            colour(RED, 0);
            clear();
            e = call(GDRAW + 20, mode ? 0x10102 : 0x10002, ros_addr(tab), 0, 0, 0, 0, 1, at(O_MATRIX), NULL);
            e |= call(GDRAW + 4, at(O_LINE), 0, at(O_MATRIX), 0, 0, at(O_CAPS), 0, 0, NULL);
            e |= fill(GDRAW + 2, O_PATHB, 0xB0);
            okg &= !e;
            for (int x = 3; x <= 29 && okg; x++) {
                uint32_t v = pix(x, 16), i = (v & 0xFF) | ((v >> 16) & 0xFF) << 8;
                okg &= (v & 0xFF00) == 0x4000 && (mode ? i >> 3 : i) == row16[x];
            }
            for (int y = 3; mode && y <= 29; y++) {
                uint32_t v = pix(16, y);
                okd &= (v & 7) == 7 - ((uint32_t)y & 7);
            }
            e = fill(GDRAW + 2, O_PATHB, 0xB0);
            okflat &= !e && pix(16, 16) == 0xFF;
        }
        check(okg && okd && okflat, "GDraw: a graduated fill (SetFillStyle 2, bit 16; its axis by Stroke) -- "
              "GDraw 3.12's steps exactly, its dither (mode 1), then flat", "%s", row_text(16));
        ros_rma_free(tab);
        call(GDRAW + 20, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    }

    /* #137: an anti-aliased fill past the window's right and top: Draw
     * leaves those edges out, and the shape goes on to the window's edge */
    {
        const int32_t big[] = { 2, P(-10), P(-10), 8, P(42), P(-10), 8, P(42), P(42), 8, P(-10), P(42), 5, 0 };
        path_put(O_PATHH, big, sizeof big / sizeof big[0]);
        colour(RED, 0);
        clear();
        e = fill(GDRAW + 2, O_PATHH, 0xB0);
        int all = !e;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                all &= pix(x, y) == 0xFF;
        check(all, "GDraw: an anti-aliased fill past the window's right and top edges covers it all",
              "%s", row_text(16));
        make_paths();
    }
    e = call(GDRAW + 22, 1, 0, 0, 0, 0, 0, 0, 0, NULL);
    e2 = call(GDRAW + 22, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    check(!e && !e2, "GDraw: SetPrintFlag 1, then 0 -- recorded", "&%X &%X", e, e2);

    /* ---- regions ---- */
    colour(RED, 0);
    uint32_t reg = at(O_REGION), r0 = 0;
    ros_st32(reg, 0), ros_st32(reg + 4, REGION_SIZE);
    /* a clip region, pixels (2, 2)-(16, 30), non-AA */
    const int32_t cp[] = { 2, P(2), P(2), 8, P(16), P(2), 8, P(16), P(30), 8, P(2), P(30), 5, 0 };
    path_put(O_BUF1, cp, sizeof cp / sizeof cp[0]);
    e = call(GDRAW + 14, at(O_BUF1), 0, at(O_MATRIX), 0, reg, 0, 0, 0, &r0);
    check(!e && r0 == reg, "GDraw: ClipPath into a buffer -- R0 the region", "&%X &%X", e, r0);
    e = call(GDRAW + 19, reg, 0, 0, 0, 0, 0, 0, 0, NULL);
    uint32_t got = 0;
    e2 = call(GDRAW + 24, 0, 0, 0, 0, 0, 0, 0, 0, &got);
    check(!e && !e2 && got == reg, "GDraw: SetClipRegion, GetClipRegion -- the region", "&%X", got);
    clear();
    e = fill(GDRAW + 2, O_PATHR, 0xB0);
    row_want(want, 0xFFFFFF, 10, 15, 0xFF, -1, 0);
    check(!e && row_is(16, want), "GDraw: Fill R, &B0, clipped to the region -- x 10-15 only", "%s",
          row_text(16));
    clear();
    e = fill(GDRAW + 2, O_PATHR, 0x30);
    check(!e && row_is(16, want), "GDraw: Fill R, &30, clipped to the region -- x 10-15 only", "%s",
          row_text(16));
    call(GDRAW + 18, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    e2 = call(GDRAW + 24, 0, 0, 0, 0, 0, 0, 0, 0, &got);
    check(!e2 && got == 0, "GDraw: ClearClipRegion -- no region", "&%X", got);
    /* an anti-aliased region of R, filled 4 pixels to the right */
    e = call(GDRAW + 12, at(O_PATHR), 0xB0 | 0x18000000, at(O_MATRIX), 0, 0, 0, 0, reg, &r0);
    clear();
    e2 = call(GDRAW + 21, reg, 0, 8, 0, 0, 0, 0, 0, NULL);
    row_want(want, 0xFFFFFF, 14, 23, 0xFF, 24, 0x4040FF);
    check(!e && r0 == reg && !e2 && row_is(16, want),
          "GDraw: ProcessClipPath R, &B0, then FillRegion 8 OS units right -- as #2, 4 pixels on",
          "&%X &%X %s", e, e2, row_text(16));
    e = call(GDRAW + 12, at(O_PATHR), 0xB0 | 0x18000000, at(O_MATRIX), 0, 0, 0, 0, 0, &r0);
    clear();
    e2 = call(GDRAW + 21, r0, 0, 0, 0, 0, 0, 0, 0, NULL);
    row_want(want, 0xFFFFFF, 10, 19, 0xFF, 20, 0x4040FF);
    check(!e && r0 && !e2 && row_is(16, want),
          "GDraw: ProcessClipPath to GDraw's own buffer (R7 = 0), then FillRegion -- as #2",
          "&%X &%X %s", e, e2, row_text(16));
    ros_st32(reg + 4, 40);
    e = call(GDRAW + 14, at(O_PATHR), 0, at(O_MATRIX), 0, reg, 0, 0, 0, NULL);
    check(e == 0x987, "GDraw: ClipPath into a buffer too small -- Draw's \"Output path full\"",
          "&%X %s", e, last_err);
    e = call(GDRAW + 21, 0xFFFFFFFFu, 0, 0, 0, 0, 0, 0, 0, NULL);
    e2 = call(GDRAW + 21, reg, 1, 0, 0, 0, 0, 0, 0, NULL);
    check(!e && e2 == 0x982, "GDraw: FillRegion -1 does nothing; flags other than bit 6 -- &982",
          "&%X &%X", e, e2);

    /* ---- DrawV: while GDraw has it, Draw's SWIs are GDraw's ---- */
    uint32_t flag = 0;
    e = call(GDRAW + 62, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    e2 = call(GDRAW + 61, 0, 0, 0, 0, 0, 0, 0, 0, &flag);
    check(!e && !e2 && flag, "GDraw: reason 62 claims DrawV; reason 61 says so", "&%X", flag);
    e = call(GDRAW + 62, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    check(!e, "GDraw: reason 62 again -- no error", "&%X", e);
    clear();
    e = fill(DRAW + 2, O_PATHR, 0xB0);
    row_want(want, 0xFFFFFF, 10, 19, 0xFF, 20, 0x4040FF);
    check(!e && row_is(16, want), "GDraw: Draw_Fill &B0 while GDraw has DrawV -- anti-aliased, as #2", "%s",
          row_text(16));
    e = call(GDRAW + 63, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    e2 = call(GDRAW + 61, 0, 0, 0, 0, 0, 0, 0, 0, &flag);
    uint32_t e3b = call(GDRAW + 63, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    check(!e && !e2 && !flag && !e3b, "GDraw: reason 63 releases DrawV; again, no error", "&%X", flag);
    clear();
    e = fill(DRAW + 2, O_PATHR, 0x30);
    row_want(want, 0xFFFFFF, 10, 20, 0xFF, -1, 0);
    check(!e && row_is(16, want), "GDraw: Draw_Fill &30 after release -- Draw's own", "%s", row_text(16));

    to_screen();
}

static void depth16(void)
{
    uint32_t sp = make_sprite(4);
    to_sprite(sp);
    colour(RED, 0);
    check(rect_row(0xB0, 0xFFFF, 0x1F, 19, 20, 0x211F),
          "GDraw #10: Fill R, &B0, 16 bpp -- row 15, x 10-19 &001F, x 20 &211F", "%s", row_text(16));
    to_screen();
}

static void depth8(void)
{
    uint32_t sp = make_sprite(3);
    to_sprite(sp);
    colour(RED, 0);
    check(rect_row(0xB0, 0xFF, 0x49, 19, 20, 0x77),
          "GDraw #11: Fill R, &B0, red, 8 bpp -- row 15, x 10-19 &49, x 20 &77", "%s", row_text(16));
    check(rect_row(0x30, 0xFF, 0x49, 20, -1, 0), "GDraw #11: Fill R, &30, red, 8 bpp -- x 10-20 &49",
          "%s", row_text(16));
    to_screen();
}

/* #12: black over white at 4, 2 and 1 bpp: colour numbers blended */
static void low_depth(uint32_t l2, uint32_t white, uint32_t x20, uint32_t top, uint32_t bottom)
{
    uint32_t sp = make_sprite(l2);
    to_sprite(sp);
    uint32_t want[19];
    char what[160];
    colour(BLACK, 0);
    int ok = rect_row(0xB0, white, 0, 19, 20, x20);
    for (int y = 2; y <= 28; y++) {
        int row = 31 - y;
        uint32_t w = row == 6 ? top : row == 26 ? bottom : row >= 7 && row <= 25 ? 0 : white;
        ok &= pix(15, y) == w;
    }
    snprintf(what, sizeof what, "GDraw #12: Fill R, &B0, black, %u bpp -- x 20 %u; column 15 rows 6 and 26: %u, %u",
             1u << l2, x20, top, bottom);
    check(ok, what, "row %s; column %s", row_text(16), col_text(15, 3, 29));
    clear();
    uint32_t e = fill(GDRAW + 2, O_PATHR, 0x30);
    row_want(want, white, 10, 20, 0, -1, 0);
    snprintf(what, sizeof what, "GDraw #12: Fill R, &30, black, %u bpp -- 0 at x 10-20", 1u << l2);
    check(!e && row_is(16, want), what, "%s", row_text(16));
    colour(RED, 0);
    clear();
    e = fill(GDRAW + 2, O_PATHR, 0xB0);
    row_want(want, white, 0, -1, 0, -1, 0);
    snprintf(what, sizeof what, "GDraw: Fill R, &B0, red, %u bpp -- red is white's colour number: no change",
             1u << l2);
    check(!e && row_is(16, want), what, "%s", row_text(16));
    to_screen();
}

/* #23-#25: the path SWIs' buffers byte for byte as Draw's */
static void paths(void)
{
    static const struct { const char *what; uint32_t n; } c[] = {
        { "GDraw #23: ProcessPath T, flags &18000000, to a buffer -- as Draw's", 0 },
        { "GDraw #24: FlattenPath T, flatness 512 -- as Draw's", 8 },
        { "GDraw #25: TransformPath T by a rotation and translation -- as Draw's", 10 },
    };
    for (unsigned k = 0; k < 3; k++) {
        uint32_t e[2], r0[2];
        for (int w = 0; w < 2; w++) {
            uint32_t chunk = w ? GDRAW : DRAW, buf = at(w ? O_BUF2 : O_BUF1);
            memset(mem + (w ? O_BUF2 : O_BUF1), 0xAA, BUF_SIZE);
            ros_st32(buf, 0), ros_st32(buf + 4, BUF_SIZE - 8);
            if (c[k].n == 0)
                e[w] = call(chunk, at(O_PATHT), 0x18000000, at(O_MATRIX), 0, 0, 0, 0, buf, &r0[w]);
            else if (c[k].n == 8)
                e[w] = call(chunk + 8, at(O_PATHT), buf, 512, 0, 0, 0, 0, 0, &r0[w]);
            else
                e[w] = call(chunk + 10, at(O_PATHT), buf, at(O_MAT2), 0, 0, 0, 0, 0, &r0[w]);
        }
        int same = !memcmp(mem + O_BUF1, mem + O_BUF2, BUF_SIZE);
        check(!e[0] && !e[1] && same && r0[0] - at(O_BUF1) == r0[1] - at(O_BUF2), c[k].what,
              "errors &%X &%X, same %d", e[0], e[1], same);
    }
}

void ros_selftest_gdraw(void)
{
    ros_console_printf("GDraw (native): the vectors measured on RISC OS 5.30\n");
    const struct ros_module *m = NULL;
    for (m = ros_module_first(); m && strcmp(m->title, "GDraw"); m = m->next)
        ;
    check(m && m->swi_chunk == GDRAW, "GDraw: in the ROM, SWI chunk &44540", NULL);
    if (!m)
        return;
    mem = ros_rma_alloc(MEM_SIZE);
    if (!mem) {
        check(0, "GDraw: RMA for the tests", NULL);
        return;
    }
    base = ros_addr(mem);
    make_paths();
    uint32_t r[10] = { GDRAW + 2, at(O_BUF1), 64 };
    uint32_t e = swi(OS_SWINumberToString, r);
    check(!e && !strcmp((char *)mem + O_BUF1, "GDraw_Fill"), "GDraw: SWI &44542 is GDraw_Fill", "%s",
          (char *)mem + O_BUF1);
    fresh();
    uint32_t sp = make_sprite(5);
    to_sprite(sp);
    colour(RED, 0);
    errors();
    to_screen();
    depth32();
    depth16();
    depth8();
    low_depth(2, 15, 4, 11, 8);
    low_depth(1, 3, 1, 2, 2);
    low_depth(0, 1, 0, 1, 1);
    paths();
    ros_rma_free(mem);
}
