/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_graphicsv.c: DRMVideo, the GraphicsV driver, against GraphicsV
 * as RISC OS 5 defines it (hdr/GraphicsV, the PRM).
 *
 * GraphicsV is called as the kernel's VDU drivers call it: a register
 * block through the vector, the reason in R4, R4 = 0 on return when a
 * driver claimed it.  What each call did is then read where the host
 * reads it: the screen block at the end of screen memory (screen.h), which
 * is all the Metal window knows of the mode.
 *
 * In the hosted build there is no display. DRMVideo then does not claim
 * GraphicsV, and there is nothing to check.
 */
#include <string.h>

#include "drmvideo.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/screen.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

enum {
    GV_SET_MODE = 2, GV_UPDATE_POINTER = 5, GV_SET_DMA_ADDRESS = 6, GV_VET_MODE = 7,
    GV_DISPLAY_FEATURES = 8, GV_FRAMESTORE_ADDRESS = 9, GV_WRITE_PALETTE_ENTRIES = 11,
    GV_READ_PALETTE_ENTRY = 12, GV_PIXEL_FORMATS = 17, GV_READ_INFO = 18, GV_VET_MODE2 = 19,
};
#define MF_FULL_PALETTE (1u << 7)
#define MF_RGB          (1u << 14)

/* One GraphicsV call: 1 if a driver claimed it.  r holds R0-R3 in, and
 * R0-R3 out. */
static int gv(uint32_t reason, uint32_t r[4])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 4 * sizeof r[0]);
    s.r[4] = reason;                        /* driver 0, head 0 */
    int claimed = ros_vector_call(ROS_GRAPHICSV, &s) && s.r[4] == 0;
    memcpy(r, s.r, 4 * sizeof r[0]);
    return claimed;
}

/* A VIDC list, type 3, in the RMA: timings as a monitor definition file
 * would give them, then the control list. */
static uint32_t list;

static uint32_t vidc(uint32_t w, uint32_t h, uint32_t log2bpp, uint32_t flags, uint32_t extra)
{
    uint32_t words[] = { 3, log2bpp, 32, 48, 0, w, 0, 16, 3, 13, 0, h, 0, 3,
                         w * h * 60 / 1000, 0 };
    for (unsigned i = 0; i < sizeof words / 4; i++)
        ros_st32(list + 4 * i, words[i]);
    uint32_t at = list + sizeof words;
    ros_st32(at, 15), ros_st32(at + 4, log2bpp == 5 ? 0xFFFFFFFFu : (1u << (1u << log2bpp)) - 1);
    ros_st32(at + 8, 16), ros_st32(at + 12, flags);
    ros_st32(at + 16, 14), ros_st32(at + 20, extra);
    ros_st32(at + 24, 0xFFFFFFFFu);
    return list;
}

static const struct ros_screen_block *block(const struct ros_display *d)
{
    return ros_ptr(d->base + d->size);
}

void ros_selftest_graphicsv(void)
{
    const struct ros_display *d = drmvideo_display();
    if (!d) {
        ros_console_printf("  --    GraphicsV: no display, so DRMVideo is idle\n");
        return;
    }
    const uint32_t start_w = d->width, start_h = d->height;
    list = ros_addr(ros_rma_alloc(256));
    uint32_t r[4];

    /* ---- what the driver says of itself ---- */
    memset(r, 0, sizeof r);
    check(gv(GV_DISPLAY_FEATURES, r) && (r[0] & 0x3A) == 0x3A && r[1] == 0x3F,
          "GraphicsV: DisplayFeatures -- hardware pointer, own framestore, 1 to 32 bpp",
          "flags &%X, depths &%X", r[0], r[1]);
    memset(r, 0, sizeof r);
    int claimed = gv(GV_PIXEL_FORMATS, r);
    check(claimed && r[1] == 7 && ros_in_arena(ros_ptr(r[0])) && ros_ld32(r[0] + 12 * 4 + 4) ==
          (MF_FULL_PALETTE | MF_RGB),
          "GraphicsV: PixelFormats -- seven, in the arena, 16 bpp as RGB565",
          "%u formats at &%08X", r[1], r[0]);
    char *name = ros_rma_alloc(32);
    r[0] = 2, r[1] = ros_addr(name), r[2] = 32, r[3] = 0;
    check(gv(GV_READ_INFO, r) && strcmp(name, "DRMVideo") == 0 && r[2] == 32 - 9,
          "GraphicsV: ReadInfo -- the driver's name", "\"%.31s\", R2 %d", name, (int)r[2]);
    r[0] = 2, r[1] = ros_addr(name), r[2] = 4, r[3] = 0;
    check(gv(GV_READ_INFO, r) && (int32_t)r[2] == 4 - 9,
          "GraphicsV: ReadInfo -- a short buffer says how short", "R2 %d", (int)r[2]);
    ros_rma_free(name);

    /* ---- vetting ---- */
    r[0] = vidc(640, 480, 5, 0, 0);
    check(gv(GV_VET_MODE, r) && r[0] == 0, "GraphicsV: VetMode -- 640 x 480, 32 bpp", "R0 %u", r[0]);
    r[0] = vidc(640, 480, 4, 0, 0);
    check(gv(GV_VET_MODE, r) && r[0] != 0,
          "GraphicsV: VetMode -- refuses 32K colours, which the host cannot decode", NULL);
    r[0] = vidc(640, 480, 4, MF_FULL_PALETTE | MF_RGB, 0);
    check(gv(GV_VET_MODE, r) && r[0] == 0, "GraphicsV: VetMode -- 64K colours, RGB565", NULL);
    r[0] = vidc(d->max_width + 8, 480, 5, 0, 0);
    check(gv(GV_VET_MODE, r) && r[0] != 0,
          "GraphicsV: VetMode -- refuses a mode wider than the display", "max %u", d->max_width);
    r[0] = vidc(640, 480, 3, MF_FULL_PALETTE, 0);
    check(gv(GV_VET_MODE2, r) && r[0] == 3,
          "GraphicsV: VetMode2 -- supported, framestore made at SetMode", "R0 %u", r[0]);

    /* ---- an 8 bpp mode: the block, the framestore, the palette ---- */
    uint32_t gen = block(d)->generation;
    r[0] = vidc(640, 480, 3, MF_FULL_PALETTE, 0);
    gv(GV_SET_MODE, r);
    const struct ros_screen_block *b = block(d);
    check(b->magic == ROS_SCREEN_MAGIC && b->generation != gen && b->xres == 640 &&
              b->yres == 480 && b->bpp == 8 && b->pitch == 640,
          "GraphicsV: SetMode -- 640 x 480 x 8 bpp, described to the host",
          "%ux%u %u bpp pitch %u", b->xres, b->yres, b->bpp, b->pitch);
    memset(r, 0, sizeof r);
    check(gv(GV_FRAMESTORE_ADDRESS, r) && r[0] == ROS_SCREEN_BASE && r[1] >= 640 * 480,
          "GraphicsV: FramestoreAddress -- screen memory in the arena", "&%08X, %u bytes", r[0],
          r[1]);
    uint32_t *pal = ros_rma_alloc(256 * 4);
    for (uint32_t i = 0; i < 256; i++)
        pal[i] = (i << 24) | ((255 - i) << 16) | ((i * 7 & 255) << 8) | 0x10;    /* &BBGGRRSS */
    r[0] = 0, r[1] = ros_addr(pal), r[2] = 0, r[3] = 256;
    gv(GV_WRITE_PALETTE_ENTRIES, r);
    int same = 1;
    for (uint32_t i = 0; i < 256; i++)
        same &= b->palette[i] == pal[i] >> 8;
    check(same, "GraphicsV: WritePaletteEntries -- the host's palette, as &00BBGGRR", NULL);
    r[0] = 0, r[1] = 0, r[2] = 77, r[3] = 0;
    check(gv(GV_READ_PALETTE_ENTRY, r) && r[1] == pal[77],
          "GraphicsV: ReadPaletteEntry -- what was written", "&%08X", r[1]);
    ros_rma_free(pal);

    r[0] = 0, r[1] = d->base + 10 * d->stride, r[2] = r[3] = 0;
    gv(GV_SET_DMA_ADDRESS, r);
    check(b->yoffset == 10 && b->xoffset == 0,
          "GraphicsV: SetDMAAddress -- the display's start moves ten rows", "offset %u,%u",
          b->xoffset, b->yoffset);
    r[0] = 0, r[1] = d->base;
    gv(GV_SET_DMA_ADDRESS, r);

    /* ---- the pointer: a 2 bpp shape, in the pointer palette ---- */
    uint32_t *ppal = ros_rma_alloc(16);
    ppal[0] = 0, ppal[1] = 0xFF000000u, ppal[2] = 0x0000FF00u, ppal[3] = 0x00FF0000u;
    r[0] = 2, r[1] = ros_addr(ppal), r[2] = 0, r[3] = 4;
    gv(GV_WRITE_PALETTE_ENTRIES, r);
    uint8_t *shape = ros_rma_alloc(8 + 8 * 16);
    uint8_t *image = shape + 8;
    memset(shape, 0, 8 + 8 * 16);
    shape[0] = 4, shape[1] = 16;                /* 16 x 16 */
    ros_st32(ros_addr(shape) + 4, ros_addr(image));
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (x <= y)
                image[y * 8 + x / 4] |= (uint8_t)((x == 0 || x == y || y == 15 ? 1 : 2)
                                                  << ((x % 4) * 2));
    r[0] = 3, r[1] = 100, r[2] = 100, r[3] = ros_addr(shape);
    check(gv(GV_UPDATE_POINTER, r), "GraphicsV: UpdatePointer -- a shape, on the cursor plane",
          NULL);
    r[0] = 1, r[1] = 120, r[2] = 90;
    check(gv(GV_UPDATE_POINTER, r), "GraphicsV: UpdatePointer -- moved", NULL);
    r[0] = 0;
    gv(GV_UPDATE_POINTER, r);
    ros_rma_free(shape);
    ros_rma_free(ppal);

    /* ---- 32 bpp both ways round, and a row with extra bytes ---- */
    r[0] = vidc(640, 480, 5, 0, 64);
    gv(GV_SET_MODE, r);
    b = block(d);
    check(b->bpp == 32 && b->pixo == 1 && b->pitch == 640 * 4 + 64,
          "GraphicsV: SetMode -- 32 bpp &xBGR, 64 extra bytes a row", "pixo %u pitch %u",
          b->pixo, b->pitch);
    r[0] = vidc(d->max_width, d->max_height, 3, MF_FULL_PALETTE, 0);
    gv(GV_SET_MODE, r);
    b = block(d);
    check(b->xres == d->max_width && b->yres == d->max_height && d->width == d->max_width,
          "GraphicsV: SetMode -- the largest size the display lists", "%ux%u, wanted %ux%u",
          b->xres, b->yres, d->max_width, d->max_height);
    r[0] = vidc(start_w, start_h, 5, MF_RGB, 0);
    gv(GV_SET_MODE, r);
    b = block(d);
    check(b->bpp == 32 && b->pixo == 0 && b->xres == start_w && d->width == start_w,
          "GraphicsV: SetMode -- back to the start-up mode, 32 bpp &xRGB", "%ux%u pixo %u",
          b->xres, b->yres, b->pixo);
    ros_rma_free(ros_ptr(list));
}

/* After the self-test, with rosgd.gvdemo: a palettised mode and a pointer
 * set through GraphicsV, left on screen for a person, or a screenshot, to
 * look at.  Colour bars in 8 bpp: every palette entry, in columns. */
void ros_graphicsv_demo(void)
{
    const struct ros_display *d = drmvideo_display();
    if (!d)
        return;
    list = ros_addr(ros_rma_alloc(256));
    uint32_t r[4];
    r[0] = vidc(d->width, d->height, 3, MF_FULL_PALETTE, 0);
    gv(GV_SET_MODE, r);
    uint32_t *pal = ros_rma_alloc(256 * 4);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t red = i & 0xE0, green = (i << 3) & 0xE0, blue = (i << 6) & 0xC0;
        pal[i] = blue << 24 | green << 16 | red << 8;
    }
    r[0] = 0, r[1] = ros_addr(pal), r[2] = 0, r[3] = 256;
    gv(GV_WRITE_PALETTE_ENTRIES, r);
    for (uint32_t y = 0; y < d->height; y++)
        for (uint32_t x = 0; x < d->width; x++)
            ros_st8(d->base + y * d->stride + x, (x * 16 / d->width) * 16 + y * 16 / d->height);

    uint32_t *ppal = ros_rma_alloc(16);
    ppal[1] = 0x00000000u, ppal[2] = 0xFFFF0000u, ppal[3] = 0x0000FF00u;   /* black, cyan, red */
    r[0] = 2, r[1] = ros_addr(ppal), r[2] = 0, r[3] = 4;
    gv(GV_WRITE_PALETTE_ENTRIES, r);
    uint8_t *shape = ros_rma_alloc(8 + 8 * 32);
    uint8_t *image = shape + 8;
    memset(shape, 0, 8 + 8 * 32);
    shape[0] = 8, shape[1] = 24;
    ros_st32(ros_addr(shape) + 4, ros_addr(image));
    for (int y = 0; y < 24; y++)                 /* an arrow: outline 1, fill 2 */
        for (int x = 0; x <= y / 2 + 1 && x < 32; x++)
            image[y * 8 + x / 4] |= (uint8_t)((x == 0 || x == y / 2 + 1 || y == 23 ? 1 : 2)
                                              << ((x % 4) * 2));
    r[0] = 3, r[1] = d->width / 2, r[2] = d->height / 2, r[3] = ros_addr(shape);
    gv(GV_UPDATE_POINTER, r);
    ros_console_printf("rosgd: GraphicsV demo: %ux%u, 8 bpp, 256 colours, pointer at %u,%u\n",
                       d->width, d->height, d->width / 2, d->height / 2);
}
