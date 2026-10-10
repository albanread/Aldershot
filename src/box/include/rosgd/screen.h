/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* screen.h -- the screen block: how the host learns what screen memory holds.
 *
 * Screen memory is a DRM dumb buffer on virtio-gpu (platform/display_drm.c).
 * Linux's virtio-gpu driver declares every scanout XRGB8888, but a RISC OS
 * mode can be 1 to 32 bits per pixel, palettised, in either byte order. So
 * the runtime fills the buffer in the mode's own format and describes it in
 * the buffer's last 4 KB: this block.
 *
 * The host's Metal window (BOX's QEMU, ui/metal-virtio-gpu.c) reads
 * the buffer's pages straight out of guest RAM every frame, reads the block,
 * and decodes the pixels on the GPU, as it does the Pi's framebuffer, with
 * the block in place of the Pi's mailbox. Nothing in the guest has to flush
 * or convert anything. A display that does not know the block (QEMU's own
 * windows) shows the buffer as XRGB8888, which is right only for 32 bpp
 * modes with blue in the low byte.
 *
 * The words are little-endian. The QEMU side keeps its own copy of this
 * layout, so change both or neither.
 */
#ifndef ROSGD_SCREEN_H
#define ROSGD_SCREEN_H

#include <stdint.h>

#define ROS_SCREEN_MAGIC   0x44475352u     /* "RSGD" */
#define ROS_SCREEN_VERSION 1u
#define ROS_SCREEN_BLOCK   4096u           /* the last 4 KB of the buffer */

struct ros_screen_block {
    uint32_t magic;             /* ROS_SCREEN_MAGIC, written last */
    uint32_t version;           /* ROS_SCREEN_VERSION */
    uint32_t generation;        /* moves on every mode change */
    uint32_t xres, yres;        /* visible pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bpp;               /* 1, 2, 4, 8, 16 (RGB565) or 32 */
    uint32_t pixo;              /* 32 bpp: 1 red in the low byte (&xBGR,
                                   RISC OS's default), 0 blue (&xRGB) */
    uint32_t xoffset, yoffset;  /* pan: the visible area's top left */
    /* The pointer, for a compositor that shows this screen (rosgd.display=
     * compositor). It is given as: its image's top left in the screen's
     * pixels, whether it is shown, a count that moves when the image
     * changes, where the image is (64 x 64 words, &AARRGGBB, at that offset
     * from the buffer's start; 0 means no image here), and its hot spot,
     * the shape's active point (x in bits 0-15, y in 16-31) */
    int32_t ptr_x, ptr_y;
    uint32_t ptr_shown, ptr_shape, ptr_image;
    uint32_t ptr_hot;
    uint32_t palette[256];      /* 0x00BBGGRR, for 8 bpp and below */
};

_Static_assert(sizeof(struct ros_screen_block) == 64 + 1024, "screen block layout");

/* The external windows, for the compositor: Wimp windows whose work area
 * is a Linux program's Wayland window. They come after the palette, at
 * ROS_SCREEN_EXT from the block's start. The Wimp writes them at its safe
 * points when anything in them has changed, with seq odd while it writes
 * and even when done. A reader reads them, then reads seq again, and takes
 * them only if seq was even and is unchanged.
 *
 * For each window, the block has its compositor's id and where the
 * toplevel's top left goes, in the screen's pixels (y down). Then come the
 * rectangles of the screen through which it is seen, which is what is left
 * of its work area once the windows in front and the screen's edges have
 * taken their part, in pixels, x0 y0 inclusive, x1 y1 exclusive.
 * Everywhere else the desktop is on top. */
#define ROS_SCREEN_EXT       (64u + 1024u)
#define ROS_EXT_WINDOWS      16u
#define ROS_EXT_RECTS        160u

struct ros_ext_window {
    uint32_t id;                /* the compositor's, 1 up */
    int16_t x, y;               /* the toplevel's top left */
};

struct ros_ext_rect {
    uint32_t id;
    int16_t x0, y0, x1, y1;
};

struct ros_screen_ext {
    uint32_t seq;
    uint32_t nwindows, nrects;
    uint32_t reserved;
    struct ros_ext_window window[ROS_EXT_WINDOWS];
    struct ros_ext_rect rect[ROS_EXT_RECTS];
};

_Static_assert(ROS_SCREEN_EXT + sizeof(struct ros_screen_ext) <= ROS_SCREEN_BLOCK, "screen block room");

#endif
