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

/* The presenter's area (design 37, 3.10): written by the PC's GPU
 * presenter, which shows this screen, and read by the box.  magic says a
 * presenter has written it; beat moves at every pass of the presenter's
 * loop (many times a second), so one that has stopped is seen to have;
 * caps says what it draws itself.  A reader takes the presenter as
 * running only while beat moves. */
#define ROS_SCREEN_PRESENT   3200u
#define ROS_PRESENT_MAGIC    0x53455250u    /* "PRES" */
#define ROS_PRESENT_SURFACES 1u             /* caps: the surface table below */
struct ros_screen_present {
    uint32_t magic;
    uint32_t state;             /* 2 running (design 37, 3.7) */
    uint32_t beat;
    uint32_t caps;
};

/* The surfaces the presenter draws itself (design 37, 3.10), after the
 * external windows' table: icons whose pictures the GPU draws over the
 * desktop, so that screen memory under them stays still.  The Wimp writes
 * the table at its safe points when anything in it has changed, with seq
 * odd while it writes, as for the external windows.  Each surface is a
 * kind (ROS_SURF_*) and the kind's parameter, the icon's box on the
 * screen in its pixels (y down), the parts of the box that can be seen
 * (x0 y0 inclusive, x1 y1 exclusive), and up to 32 bytes the kind
 * draws from: a monitor's last 24 seconds, in 255ths, oldest first. */
#define ROS_SCREEN_SURF      (ROS_SCREEN_PRESENT + 16u)
#define ROS_SURFACES         6u
#define ROS_SURF_RECTS       8u
#define ROS_SURF_CLOCK       1u             /* DeskClock's face; param: its style */
#define ROS_SURF_MONITOR     2u             /* DeskMeter's graph; param: its style */
#define ROS_SURF_DATA        32u
struct ros_surface {
    uint32_t kind, param;
    int16_t x, y, w, h;
    uint32_t nrects;
    int16_t rect[ROS_SURF_RECTS][4];
    uint8_t data[ROS_SURF_DATA];
};
struct ros_screen_surfaces {
    uint32_t seq, n;
    uint32_t v2_offset;         /* block v2 (below): where in the buffer, 0 none */
    uint32_t v2_size;
    struct ros_surface s[ROS_SURFACES];
};

/* Block v2 (design 37, interim status 5.1 and 5.2): an area of its own in
 * the buffer, before the pointer image and the 4 KB block, found through
 * the surfaces table's v2_offset -- which readers of version 1 never used,
 * so they stay right.  The Wimp says what it drew; the presenter uploads
 * that and compares nothing.
 *
 * frame is a seqlock: the Wimp makes it odd when it begins to change the
 * screen after a publish, and even when it has published what changed.  A
 * reader takes a copy while frame is even and the same before and after.
 * The damage is the rectangles the Wimp handed to tasks to redraw, drew
 * itself, or copied to (screen pixels, y down from the top, half-open);
 * full set means the whole screen, for a mode change, the command window,
 * or a list that overflowed. */
#define ROS_V2_MAGIC   0x32565352u          /* "RSV2" */
#define ROS_V2_SIZE    65536u
#define ROS_V2_DAMAGE  256u

struct ros_v2_rect { int16_t x0, y0, x1, y1; };

/* The surfaces (design 37, interim status 5.2): one list, in the Wimp's
 * stack order, back to front, written with the damage under the same
 * seqlock.  An entry is an icon the presenter draws (the clock, the
 * meter: ROS_SURF_CLOCK, ROS_SURF_MONITOR, as the table at
 * ROS_SCREEN_SURF has them), or a window whose chrome RISC OS draws into
 * a backing of its own and whose content may be a Wayland buffer
 * (ROS_SURF_WINDOW: Sprint 3).  Its visible rectangles and its damage
 * are ranges in the pools below, screen pixels, y down, half-open.
 * frame is the v2 frame at which the entry last changed: a reader
 * touches a surface only when it moves. */
#define ROS_V2_SURFACES  128u            /* every window, child, menu and page (Sprint 5) */
#define ROS_V2_RECTS     2048u
#define ROS_V2_SDAMAGE   1024u
#define ROS_SURF_WINDOW  3u                 /* kind: a window, its chrome in a backing */
#define ROS_SURF_PAGE    4u                 /* kind: an external window's page (a Wayland
                                               surface: param, the compositor's id), placed
                                               at x,y and shown in its rects -- straight after
                                               its window's entry, so the page is drawn in the
                                               stack's order (design 37, Sprint 5) */
#define ROS_SURF_WIN_CARET 1u               /* a window entry's data[0]: it has the caret, and the
                                               presenter looks at its backing for changes the
                                               damage did not carry */

struct ros_v2_surface {
    uint32_t kind, param;
    uint32_t window;            /* the Wimp window's handle; the external
                                   window's id for the compositor is in data */
    uint32_t frame;             /* the v2 frame it last changed in */
    int16_t x, y, w, h;         /* its place on the screen, screen pixels */
    uint32_t rects, nrects;     /* its visible rectangles: a range of the rect pool */
    uint32_t damage, ndamage;   /* what changed in it since its frame: likewise */
    uint32_t backing;           /* its backing's id (0: none; ROS_SURF_WINDOW: the
                                   memfd /dev/rosgd-backing-<id>, 32 bpp + alpha) */
    uint32_t stride;            /* the backing's bytes a row; for an icon surface
                                   (CLOCK, MONITOR), the rate in Hz it asks to be
                                   drawn again at, its box alone (0: when it changes) */
    int16_t shadow_dx, shadow_dy, shadow_blur, shadow_strength;   /* the theme's; 0 none */
    uint32_t shadow_rects, shadow_nrects;   /* where its shadow shows: a range of the rect pool */
    uint8_t data[ROS_SURF_DATA];            /* an icon's own (the table's data) */
};

struct ros_screen_v2 {
    uint32_t magic, version;
    uint32_t frame;             /* the seqlock */
    uint32_t full;              /* 1: all of the screen changed */
    uint32_t ndamage;
    uint32_t ack;               /* the reader's: the last frame it took.  The
                                   Wimp starts a fresh list only once the
                                   last one was taken; else it adds to it */
    uint32_t nsurfaces;         /* the list below: 0 means the table at
                                   ROS_SCREEN_SURF is all there is */
    uint32_t look;              /* the desktop's look (design 37, Sprint 6):
                                   0, or the depth the desktop was asked for,
                                   which the presenter shows the whole screen
                                   in -- the screen itself is 32 bpp.
                                   ROS_LOOK_* */
    struct ros_v2_rect damage[ROS_V2_DAMAGE];
    struct ros_v2_surface surface[ROS_V2_SURFACES];
    struct ros_v2_rect rect[ROS_V2_RECTS];
    struct ros_v2_rect sdamage[ROS_V2_SDAMAGE];
};
_Static_assert(sizeof(struct ros_screen_v2) <= ROS_V2_SIZE, "v2 area room");
#define ROS_LOOK_L2BPP(l)   ((int)((l) & 15u) - 1)  /* Log2BPP asked for, -1 none */
#define ROS_LOOK_GREY       0x200u                 /* ModeFlags' grey palette */
#define ROS_LOOK_64K        0x4000u                /* ModeFlags' 64K: 16 bpp is 5:6:5 */
#define ROS_LOOK_4K         0x10000u               /* 16 bpp of 4096 colours, 4:4:4 */
_Static_assert(ROS_SCREEN_EXT + sizeof(struct ros_screen_ext) <= ROS_SCREEN_PRESENT, "present area room");
_Static_assert(ROS_SCREEN_SURF + sizeof(struct ros_screen_surfaces) <= ROS_SCREEN_BLOCK, "surface table room");

/* The Wayland layer (design 37, 3.11).  With the GPU presenter showing the
 * screen, the WPE edition's compositor (ports/compositor, -x) draws no
 * display of its own: it draws its windows and their popups, placed as the
 * Wimp's external windows' table says, into a memfd published as
 * /run/rosgd-wayland in the WPE root (a link to /proc/<pid>/fd/<n>, a new
 * memfd when the size changes).  This header is at its start, the pixels
 * at ROS_WAYLAND_PIXELS.  The presenter shows the layer where each
 * external window is seen and where the popups are; the compositor keeps
 * its own copy of the layout, so change both or neither. */
#define ROS_WAYLAND_MAGIC   0x594C5752u     /* "RWLY" */
#define ROS_WAYLAND_POPUPS  16u
#define ROS_WAYLAND_PIXELS  4096u           /* where the pixels begin */
struct ros_wayland_layer {
    uint32_t magic;
    uint32_t seq;               /* odd while a frame is being written */
    uint32_t width, height;     /* the layer, in RISC OS pixels */
    uint32_t stride;            /* bytes a row */
    uint32_t format;            /* DRM fourcc: ARGB8888 or XRGB8888 */
    uint32_t npopups, reserved;
    int16_t popup[ROS_WAYLAND_POPUPS][4];   /* x0 y0 x1 y1: shown wherever they are */
};
_Static_assert(sizeof(struct ros_wayland_layer) <= ROS_WAYLAND_PIXELS, "layer header room");

#endif
