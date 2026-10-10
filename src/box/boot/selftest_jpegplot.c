/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_jpegplot.c: SpriteExtend's JPEG SWIs (modules/spriteextend/
 * jpeg.c): what must always hold. A JPEG made here by CompressJPEG (16 x 8,
 * 90 dpi) is read by JPEG_Info. It is plotted with JPEG_PlotScaled and
 * JPEG_PlotTransformed into a 32bpp sprite with output switched to it.
 * The checks are that the picture's colours come out scaled as asked and
 * clipped, that nothing outside it is touched, and that the errors are
 * 5.30's. The pixels against RISC OS 5.30's are tests/desktop/jpegplot's
 * (the farm). Last, a sprite is plotted by pointer with R1 not a sprite
 * area, as OvationPro does (#154).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[10])
{
    return ((const os_error *)ros_ptr(r[0]))->errnum;
}

#define W 16
#define H 8
#define SW 40
#define SH 24

/* The picture: R across, G down, B 64 */
static void pixel_of(uint32_t x, uint32_t y, uint8_t *p)
{
    p[0] = (uint8_t)(x * 255 / (W - 1));
    p[1] = (uint8_t)(y * 255 / (H - 1));
    p[2] = 64;
}

/* Output to the sprite, R0 the plot's SWI with r, output back; its V */
static int plot_into(uint32_t area, uint32_t name, uint32_t n, uint32_t r[10])
{
    uint32_t sw[10] = { 0x13C, area, name, 0 };
    swi(XOS_SpriteOp, sw);
    uint32_t back[10] = { sw[0], sw[1], sw[2], sw[3] };
    int v = swi(n, r);
    swi(XOS_SpriteOp, back);
    return v;
}

void ros_selftest_jpegplot(void)
{
    /* the JPEG */
    uint32_t buf = ros_addr(ros_rma_alloc(8192)), p = ros_addr(ros_rma_alloc(24));
    uint32_t pv[6] = { W, H, 90, 3, 90, 90 };
    memcpy(ros_ptr(p), pv, sizeof pv);
    uint8_t *row = ros_rma_alloc(W * 3);
    uint32_t st[10] = { buf, 8192, p, 0, 0 };
    int ok = !swi(XCompressJPEG_Start, st);
    uint32_t tag = st[0];
    for (uint32_t y = 0; ok && y < H; y++) {
        for (uint32_t x = 0; x < W; x++)
            pixel_of(x, y, row + 3 * x);
        uint32_t l[10] = { tag, ros_addr(row) };
        ok = !swi(XCompressJPEG_WriteLine, l);
    }
    uint32_t fin[10] = { tag };
    ok = ok && !swi(XCompressJPEG_Finish, fin);
    uint32_t len = fin[0];
    check(ok && len > 100, "JPEG plotting: a 16 x 8 JPEG from CompressJPEG to plot", "%u bytes", len);

    /* JPEG_Info */
    uint32_t in[10] = { 1, buf, len, 0x55, 0x66 };
    int v = swi(XJPEG_Info, in);
    check(!v && in[0] == 2 && in[1] == buf && in[2] == W && in[3] == H && in[4] == 90 &&
              in[5] == 90 && (in[6] & 4095) == 0,
          "JPEG_Info &49980: flags 2 (colour, \"transformed plots not supported\", a density in dpi), "
          "16 x 8, 90 x 90 dpi, R6 a whole number of pages", "V %d R0 &%X %u x %u %u %u R6 %u",
          v, in[0], in[2], in[3], in[4], in[5], in[6]);
    uint32_t in0[10] = { 2, buf, len, 0x55, 0x66 };
    v = swi(XJPEG_Info, in0);
    check(!v && in0[0] == 2 && in0[2] == len && in0[3] == 0x55,
          "JPEG_Info, flags 2: the SOF type 0 in bits 3-6; R2, R3 kept without bit 0", "R0 &%X R2 %u R3 &%X",
          in0[0], in0[2], in0[3]);
    uint32_t bad[10] = { 4, buf, len }, notj[10] = { 1, p, 24 };
    int v1 = swi(XJPEG_Info, bad), v2 = swi(XJPEG_Info, notj);
    check(v1 && errnum(bad) == 0x712 && v2 && errnum(notj) == 0x71A,
          "JPEG_Info: a reserved flag &712, not a JPEG &71A \"Incomplete or corrupt JPEG data\"",
          "&%X &%X", v1 ? errnum(bad) : 0, v2 ? errnum(notj) : 0);
    char nm[] = "JPEG_PlotScaled";
    uint32_t sn[10] = { 0 };
    char *nmr = ros_rma_alloc(sizeof nm);
    memcpy(nmr, nm, sizeof nm);
    sn[1] = ros_addr(nmr);
    v = swi(XOS_SWINumberFromString, sn);
    check(!v && sn[0] == 0x49982, "OS_SWINumberFromString \"JPEG_PlotScaled\": &49982, as ChangeFSI asks",
          "&%X", sn[0]);

    /* a 32bpp sprite to plot into */
    uint32_t asz = 16 + 44 + SW * SH * 4 + 64;
    uint32_t area = ros_addr(ros_rma_alloc(asz));
    ros_st32(area, asz), ros_st32(area + 8, 16);
    uint32_t ia[10] = { 0x109, area };
    swi(XOS_SpriteOp, ia);
    char *sname = ros_rma_alloc(4);
    strcpy(sname, "j");
    uint32_t cs[10] = { 0x10F, area, ros_addr(sname), 0, SW, SH, (6u << 27) | (90u << 14) | (90u << 1) | 1 };
    swi(XOS_SpriteOp, cs);
    uint32_t sp = area + ros_ld32(area + 8);
    uint32_t img = sp + ros_ld32(sp + 32);
#define PIX(x, y) ros_ld32(img + ((SH - 1 - (y)) * SW + (x)) * 4)
#define CLEAR() memset(ros_ptr(img), 0x5A, SW * SH * 4)

    /* 1:1 at (4, 2) pixels: the picture there, within JPEG's loss, the rest untouched */
    CLEAR();
    uint32_t ps[10] = { buf, 8, 4, 0, len, 0 };
    v = plot_into(area, ros_addr(sname), XJPEG_PlotScaled, ps);
    int close = 1, outside = 1;
    for (uint32_t y = 0; y < SH; y++)
        for (uint32_t x = 0; x < SW; x++) {
            uint32_t px = PIX(x, y);
            int in_pic = x >= 4 && x < 4 + W && y >= 2 && y < 2 + H;
            if (!in_pic) {
                if (px != 0x5A5A5A5Au)
                    outside = 0;
                continue;
            }
            uint8_t want[3];
            pixel_of(x - 4, (H - 1) - (y - 2), want);
            for (int c = 0; c < 3; c++) {
                int got = (int)((px >> (8 * c)) & 255), d = got - want[c];
                if (d < -40 || d > 40)
                    close = 0;
            }
        }
    check(!v && close && outside && ps[0] == buf && ps[5] == 0,
          "JPEG_PlotScaled &49982, 1:1 into a 32bpp sprite: the picture (&0BGR, within JPEG's loss), "
          "nothing outside it touched, registers kept", "V %d close %d outside %d", v, close, outside);

    /* 2:1: each pixel of the 1:1 plot twice each way */
    uint32_t one[W * H];
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            one[y * W + x] = PIX(4 + x, 2 + y);
    CLEAR();
    uint32_t sc = ros_addr(ros_rma_alloc(16));
    uint32_t scv[4] = { 2, 2, 1, 1 };
    memcpy(ros_ptr(sc), scv, 16);
    uint32_t p2[10] = { buf, 0, 0, sc, len, 0 };
    v = plot_into(area, ros_addr(sname), XJPEG_PlotScaled, p2);
    int doubled = 1;
    for (uint32_t y = 0; y < 2 * H; y++)
        for (uint32_t x = 0; x < 2 * W; x++)
            if (PIX(x, y) != one[(y / 2) * W + x / 2])
                doubled = 0;
    check(!v && doubled && PIX(2 * W, 0) == 0x5A5A5A5Au,
          "JPEG_PlotScaled at 2:1: each pixel of the 1:1 plot, twice each way", "V %d", v);

    /* clipped: at -4 pixels, the picture's columns from 4 */
    CLEAR();
    uint32_t pc[10] = { buf, (uint32_t)-8, 0, 0, len, 0 };
    v = plot_into(area, ros_addr(sname), XJPEG_PlotScaled, pc);
    int clipped = 1;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W - 4; x++)
            if (PIX(x, y) != one[y * W + x + 4])
                clipped = 0;
    check(!v && clipped && PIX(W - 4, 0) == 0x5A5A5A5Au,
          "JPEG_PlotScaled at x = -4 pixels: clipped at the sprite's edge, its columns from 4", "V %d", v);

    /* PlotTransformed: a scale matrix plots as PlotScaled; a rotation is refused */
    CLEAR();
    uint32_t mx = ros_addr(ros_rma_alloc(24));
    int32_t m[6] = { 2 << 16, 0, 0, 2 << 16, 0, 0 };
    memcpy(ros_ptr(mx), m, 24);
    uint32_t pt[10] = { buf, 0, mx, len, 0 };
    v = plot_into(area, ros_addr(sname), XJPEG_PlotTransformed, pt);
    int same = 1;
    for (uint32_t y = 0; y < 2 * H; y++)
        for (uint32_t x = 0; x < 2 * W; x++)
            if (PIX(x, y) != one[(y / 2) * W + x / 2])
                same = 0;
    m[1] = 1 << 16;
    memcpy(ros_ptr(mx), m, 24);
    uint32_t pr[10] = { buf, 0, mx, len, 0 };
    int vr = plot_into(area, ros_addr(sname), XJPEG_PlotTransformed, pr);
    check(!v && same && vr && errnum(pr) == 0x71B,
          "JPEG_PlotTransformed &49984: a matrix of 2 (90 dpi) plots as PlotScaled 2:1; a rotation &71B "
          "\"Transformed JPEG plotting is not supported\", as 5.30", "V %d same %d &%X", v, same,
          vr ? errnum(pr) : 0);

    /* reserved flags; PDriverIntercept */
    uint32_t pf[10] = { buf, 0, 0, 0, len, 0x1000 };
    v = plot_into(area, ros_addr(sname), XJPEG_PlotScaled, pf);
    uint32_t i1[10] = { 3 }, i2[10] = { 0 };
    swi(XJPEG_PDriverIntercept, i1);
    swi(XJPEG_PDriverIntercept, i2);
    check(v && errnum(pf) == 0x712 && i2[0] == 3,
          "JPEG_PlotScaled, a reserved flag: &712; JPEG_PDriverIntercept returns the old flags", "&%X %u",
          v ? errnum(pf) : 0, i2[0]);

    /* #154: a sprite by pointer (&2xx) to the plotting calls, with R1 not an
     * area. OvationPro gives SpriteOp &238 R1 = &FF. SpriteExtend's
     * findsprite does not look at R1 then. findsprite_inarea is used by the
     * calls that work on the area (a palette, &225), and it still says
     * "Bad address". */
    uint32_t bsz = 16 + 44 + 4 * 4 * 4 + 64;
    uint32_t barea = ros_addr(ros_rma_alloc(bsz));
    ros_st32(barea, bsz), ros_st32(barea + 8, 16);
    uint32_t ib[10] = { 0x109, barea };
    swi(XOS_SpriteOp, ib);
    strcpy(sname, "s");
    uint32_t cb[10] = { 0x10F, barea, ros_addr(sname), 0, 4, 4, (6u << 27) | (90u << 14) | (90u << 1) | 1 };
    swi(XOS_SpriteOp, cb);
    uint32_t bsp = barea + ros_ld32(barea + 8);
    for (uint32_t i = 0; i < 16; i++)
        ros_st32(bsp + ros_ld32(bsp + 32) + 4 * i, 0x00112233u);
    strcpy(sname, "j");
    CLEAR();
    uint32_t ps4[10] = { 0x234, 0xFF, bsp, 8, 4, 0, 0, 0 };
    int vs = plot_into(area, ros_addr(sname), XOS_SpriteOp, ps4);
    int at_s = (PIX(4, 2) & 0xFFFFFFu) == 0x112233u && (PIX(7, 5) & 0xFFFFFFu) == 0x112233u &&
               PIX(8, 2) == 0x5A5A5A5Au;
    CLEAR();
    uint32_t mt = ros_addr(ros_rma_alloc(24));
    int32_t mm[6] = { 1 << 16, 0, 0, 1 << 16, 20 * 256, 8 * 256 };
    memcpy(ros_ptr(mt), mm, sizeof mm);
    uint32_t tr[10] = { 0x238, 0xFF, bsp, 0, 0, 0, mt, 0 };
    int vt = plot_into(area, ros_addr(sname), XOS_SpriteOp, tr);
    int at_t = (PIX(10, 4) & 0xFFFFFFu) == 0x112233u && PIX(9, 4) == 0x5A5A5A5Au;
    uint32_t pa[10] = { 0x225, 0xFF, bsp, 0xFFFFFFFFu };
    int vp = swi(XOS_SpriteOp, pa);
    check(!vs && at_s && !vt && at_t && vp && errnum(pa) == 0xFC,
          "SpriteOp &234 and &238, a sprite pointer and R1 &FF (#154): plotted, as 5.30's findsprite; "
          "&225 the same: \"Bad address\" &FC (findsprite_inarea)", "V %d %d at %d %d &%X &%X", vs, vt, at_s,
          at_t, vs ? errnum(ps4) : vt ? errnum(tr) : 0, vp ? errnum(pa) : 0);
}
