/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* drmvideo.c -- DRMVideo, the GraphicsV driver for ROSGD, as a native module.
 *
 * RISC OS's VDU drivers own the screen's contents. A GraphicsV driver owns
 * the hardware beneath them: modes, the palette, the pointer, and where the
 * framestore is. On a Pi that is BCMVideo, talking to the GPU's mailbox.
 * Under ROSGD it is this module, talking to Linux's DRM through the
 * platform layer (platform/display_drm.c). That layer holds screen memory
 * in the mode's own pixel format and describes it to the host in a screen
 * block (screen.h). The host's Metal window does the rest.
 *
 * It is new code, written from GraphicsV as RISC OS 5 defines it
 * (hdr/GraphicsV, the PRM). It is not a translation.
 * All its state is in the RMA, reached through its private word (module.h).
 */
#include <stdio.h>
#include <string.h>

#include "drmvideo.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"

/* the screens of a mode's memory: banks for OS_Byte 112 and 113 */
#define SCREEN_BANKS 3

/* GraphicsV reason codes: R4 bits 0-15; bits 24-31 the driver. */
enum {
    GV_SET_MODE = 2, GV_SET_BLANK = 4, GV_UPDATE_POINTER = 5, GV_SET_DMA_ADDRESS = 6,
    GV_VET_MODE = 7, GV_DISPLAY_FEATURES = 8, GV_FRAMESTORE_ADDRESS = 9,
    GV_WRITE_PALETTE_ENTRY = 10, GV_WRITE_PALETTE_ENTRIES = 11, GV_READ_PALETTE_ENTRY = 12,
    GV_SELECT_HEAD = 15, GV_STARTUP_MODE = 16, GV_PIXEL_FORMATS = 17, GV_READ_INFO = 18, GV_VET_MODE2 = 19,
};
#define DRIVER 0u               /* the only driver: nothing registers others yet */

enum { PAL_NORMAL = 0, PAL_BORDER = 1, PAL_POINTER = 2 };

/* DisplayFeatures flags. */
#define FEATURE_HARDWARE_POINTER     (1u << 1)
#define FEATURE_SEPARATE_FRAMESTORE  (1u << 3)
#define FEATURE_NO_VSYNC_IRQ         (1u << 4)
#define FEATURE_VARIABLE_FRAMESTORE  (1u << 5)

/* Mode flags (Kernel/hdr/VduExt). */
#define MF_TELETEXT             (1u << 1)       /* two screens: see mode_from_list */
#define MF_HARD_SCROLL_DISABLED (1u << 6)
#define MF_FULL_PALETTE         (1u << 7)       /* with 16 bpp: 64K, RGB565 */
#define MF_GREYSCALE_PALETTE    (1u << 9)
#define MF_RGB                  (1u << 14)      /* &xRGB rather than &xBGR */
#define MF_ALPHA                (1u << 15)
#define MF_UNDERSTOOD (MF_TELETEXT | MF_HARD_SCROLL_DISABLED | MF_FULL_PALETTE | \
                       MF_GREYSCALE_PALETTE | MF_RGB | MF_ALPHA)

/* A VIDC list, type 3 (Kernel/hdr/VIDCList): words, then index/value pairs. */
enum { VL_TYPE = 0, VL_LOG2BPP = 1, VL_HDISPLAY = 5, VL_VDISPLAY = 11, VL_CONTROL = 16 };
enum { CL_EXTRA_BYTES = 14, CL_NCOLOUR = 15, CL_MODE_FLAGS = 16 };
#define CL_TERMINATOR 0xFFFFFFFFu

/* VetMode2 results. */
#define VET2_UNKNOWN_FRAMESTORE 3u

#define VERSION_BCD 0x0001u     /* 0.01 */

struct workspace {
    struct ros_display display;
    uint32_t have_display;
    uint32_t palette[256];              /* &BBGGRRSS, as written */
    uint32_t border;
    uint32_t pointer_palette[4];
    uint32_t pointer_dirty;             /* the palette changed: redraw the image */
    uint32_t blank;
    uint32_t formats[7][3];             /* GraphicsV_PixelFormats' list */
    uint32_t startup[6];                /* GraphicsV_StartupMode's mode selector */
};

struct ros_module drmvideo_module;

static struct workspace *ws(void)
{
    uint32_t w = drmvideo_module.private_word ? ros_ld32(drmvideo_module.private_word) : 0;
    return w ? ros_ptr(w) : NULL;
}

const struct ros_display *drmvideo_display(void)
{
    struct workspace *w = ws();
    return w && w->have_display ? &w->display : NULL;
}

/* ---- modes -------------------------------------------------------------- */

struct mode {
    uint32_t width, height, bpp, pixo, stride, extra, rows;
};

/* What a VIDC list asks for, if this driver can show it.  0, or -1. */
static int mode_from_list(const struct workspace *w, uint32_t list, struct mode *m)
{
    if (!list || ros_ld32(list + 4 * VL_TYPE) != 3)
        return -1;
    uint32_t log2bpp = ros_ld32(list + 4 * VL_LOG2BPP);
    if (log2bpp > 5)
        return -1;
    memset(m, 0, sizeof *m);
    m->width = ros_ld32(list + 4 * VL_HDISPLAY);
    m->height = ros_ld32(list + 4 * VL_VDISPLAY);
    m->bpp = 1u << log2bpp;

    uint32_t flags = 0, have_flags = 0;
    uint32_t item = list + 4 * VL_CONTROL;
    for (int n = 0; n < 64; n++, item += 8) {
        uint32_t index = ros_ld32(item);
        if (index == CL_TERMINATOR)
            break;
        uint32_t value = ros_ld32(item + 4);
        if (index == CL_EXTRA_BYTES)
            m->extra = value;
        else if (index == CL_MODE_FLAGS) {
            flags = value;
            have_flags = 1;
        }
    }
    if (have_flags && (flags & ~MF_UNDERSTOOD))
        return -1;              /* YCbCr, CMYK, rotation, ...
                                   NColour follows from the depth here. */
    /* Teletext: the VDU drivers keep a second screen after the first, the
     * page with its flashing characters hidden, and flash by moving the
     * display between them (SetDMAAddress). */
    m->rows = flags & MF_TELETEXT ? 2 * m->height : m->height;
    /* Screen banks (OS_Byte 112, 113): room for SCREEN_BANKS screens of the
     * mode, as a RISC OS machine's screen memory has room for several.
     * The VDU draws in one and SetDMAAddress shows another. */
    m->rows *= SCREEN_BANKS;
    /* 16 bpp: only RGB565, the one the host decodes. RISC OS's other
     * 16 bpp formats, 1:5:5:5 (32K) and BGR565, are refused. */
    if (m->bpp == 16 && (flags & (MF_FULL_PALETTE | MF_RGB)) != (MF_FULL_PALETTE | MF_RGB))
        return -1;
    m->pixo = m->bpp == 32 && !(flags & MF_RGB);     /* &xBGR: red in the low byte */
    m->stride = ((m->width * m->bpp + 31) / 32) * 4 + m->extra;
    if (m->extra % 4)
        return -1;
    if (!m->width || !m->height || !w->have_display ||
        m->width > w->display.max_width || m->height > w->display.max_height)
        return -1;
    return 0;
}

static void push_palette(struct workspace *w)
{
    uint32_t bgr[256];
    for (int i = 0; i < 256; i++)
        bgr[i] = w->palette[i] >> 8;
    ros_display_set_palette(&w->display, 0, 256, bgr);
}

/* ---- the pointer ---------------------------------------------------------
 *
 * The kernel's shape: +0 width in bytes, +1 height in rows, +4 the address
 * of the image. The image has 2 bits per pixel, lowest first, with rows
 * padded to 8 bytes, up to 32 x 32. 0 is transparent, and 1 to 3 are the
 * pointer palette's colours. DRM's cursor plane takes 64 x 64 ARGB.
 */
static void pointer_image(const struct workspace *w, uint32_t shape, uint32_t *argb)
{
    memset(argb, 0, 64 * 64 * 4);
    uint32_t width = ros_ld8(shape) * 4, height = ros_ld8(shape + 1), image = ros_ld32(shape + 4);
    if (width > 32)
        width = 32;
    if (height > 32)
        height = 32;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pix = (ros_ld8(image + y * 8 + x / 4) >> ((x % 4) * 2)) & 3;
            if (!pix)
                continue;
            uint32_t c = w->pointer_palette[pix];       /* &BBGGRRSS */
            argb[y * 64 + x] = 0xFF000000u | ((c >> 8) & 0xFF) << 16 |
                               ((c >> 16) & 0xFF) << 8 | (c >> 24);
        }
}

/* ---- ReadInfo ------------------------------------------------------------ */

/* Copy what fits of an item into the caller's buffer: R2 comes back as the
 * buffer's size less the item's, so negative when it did not fit. */
static void give(struct ros_cpu *s, const void *data, uint32_t n)
{
    uint32_t room = s->r[2];
    memcpy(ros_ptr(s->r[1]), data, n < room ? n : room);
    s->r[2] = room - n;
}

static int read_info(struct workspace *w, struct ros_cpu *s)
{
    char text[64];
    switch (s->r[0]) {
    case 0: {
        uint32_t v = VERSION_BCD << 8;
        give(s, &v, 4);
        return 1;
    }
    case 1:
    case 2:
        give(s, "DRMVideo", 9);
        return 1;
    case 3:
        snprintf(text, sizeof text, "%s (Linux DRM)", w->display.name);
        give(s, text, (uint32_t)strlen(text) + 1);
        return 1;
    case 4: {
        static const uint32_t items[] = { CL_EXTRA_BYTES, CL_NCOLOUR, CL_MODE_FLAGS,
                                          CL_TERMINATOR };
        give(s, items, sizeof items);
        return 1;
    }
    default:
        return 0;               /* 5, overlays: none, so not implemented */
    }
}

/* ---- the handler ---------------------------------------------------------- */

static int graphicsv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct workspace *w = ws();
    if (!w || !w->have_display || s->r[4] >> 24 != DRIVER)
        return ROS_VECTOR_PASS;
    struct mode m;
    uint32_t argb[64 * 64];

    switch (s->r[4] & 0xFFFF) {
    case GV_VET_MODE:
        s->r[0] = mode_from_list(w, s->r[0], &m) == 0 ? 0 : 1;
        break;

    case GV_VET_MODE2:
        if (mode_from_list(w, s->r[0], &m) != 0) {
            s->r[0] = 0;                            /* unsupported */
            break;
        }
        s->r[0] = VET2_UNKNOWN_FRAMESTORE;          /* made at SetMode */
        s->r[1] = 4;                                /* alignment */
        s->r[2] = m.extra;
        break;

    case GV_SET_MODE:
        if (mode_from_list(w, s->r[0], &m) != 0 ||
            ros_display_set_mode(&w->display, m.width, m.height, m.bpp, m.pixo, m.stride,
                                 m.rows) != 0) {
            ros_console_printf("DRMVideo: mode refused\n");
            break;
        }
        if (m.bpp <= 8)
            push_palette(w);
        break;

    case GV_SET_BLANK:
        w->blank = s->r[0] & 1;                     /* the host has no blanking */
        break;

    case GV_UPDATE_POINTER:
        /* R1, R2 the image's top left; the shape block's active point
         * (+2, +3) its hot spot */
        if (!(s->r[0] & 1)) {
            ros_display_set_pointer(ROS_POINTER_HIDE, NULL, 0, 0, 0, 0);
        } else if ((s->r[0] & 2) || w->pointer_dirty) {
            pointer_image(w, s->r[3], argb);
            ros_display_set_pointer(ROS_POINTER_SHAPE, argb, (int32_t)s->r[1], (int32_t)s->r[2],
                                    ros_ld8(s->r[3] + 2), ros_ld8(s->r[3] + 3));
            w->pointer_dirty = 0;
        } else {
            ros_display_set_pointer(ROS_POINTER_MOVE, NULL, (int32_t)s->r[1], (int32_t)s->r[2], 0, 0);
        }
        break;

    case GV_SET_DMA_ADDRESS:
        if (s->r[0] == 0)                           /* VInit: the display's start */
            ros_display_set_origin(&w->display, s->r[1]);
        break;

    case GV_DISPLAY_FEATURES:
        s->r[0] = FEATURE_HARDWARE_POINTER | FEATURE_SEPARATE_FRAMESTORE |
                  FEATURE_NO_VSYNC_IRQ | FEATURE_VARIABLE_FRAMESTORE;
        s->r[1] = 0x3F;                             /* 1, 2, 4, 8, 16, 32 bpp */
        s->r[2] = 4;
        break;

    case GV_FRAMESTORE_ADDRESS:
        s->r[0] = w->display.base;                  /* ROSGD's "physical" is the arena */
        s->r[1] = w->display.size;
        break;

    case GV_WRITE_PALETTE_ENTRY:
    case GV_WRITE_PALETTE_ENTRIES: {
        int many = (s->r[4] & 0xFFFF) == GV_WRITE_PALETTE_ENTRIES;
        uint32_t first = s->r[2], n = many ? s->r[3] : 1;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = many ? ros_ld32(s->r[1] + 4 * i) : s->r[1];
            uint32_t at = first + i;
            if (s->r[0] == PAL_NORMAL && at < 256)
                w->palette[at] = c;
            else if (s->r[0] == PAL_POINTER && at < 4) {
                w->pointer_palette[at] = c;
                w->pointer_dirty = 1;
            } else if (s->r[0] == PAL_BORDER)
                w->border = c;
        }
        /* Above 8 bpp these are gamma entries, which the host does not
         * apply: kept, to read back. */
        if (s->r[0] == PAL_NORMAL && w->display.bpp <= 8)
            push_palette(w);
        break;
    }

    case GV_READ_PALETTE_ENTRY:
        if (s->r[0] == PAL_NORMAL && s->r[2] < 256)
            s->r[1] = w->palette[s->r[2]];
        else if (s->r[0] == PAL_POINTER && s->r[2] < 4)
            s->r[1] = w->pointer_palette[s->r[2]];
        else if (s->r[0] == PAL_BORDER)
            s->r[1] = w->border;
        else
            return ROS_VECTOR_PASS;
        break;

    case GV_SELECT_HEAD:
        break;                                      /* one head */

    case GV_STARTUP_MODE:
        /* The display's own size, 16M colours in RISC OS's usual &xBGR:
         * a mode selector in place of the kernel's choice in R0. */
        s->r[0] = ros_addr(w->startup);
        break;

    case GV_PIXEL_FORMATS:
        s->r[0] = ros_addr(w->formats);
        s->r[1] = sizeof w->formats / sizeof w->formats[0];
        break;

    case GV_READ_INFO:
        if (!read_info(w, s))
            return ROS_VECTOR_PASS;
        break;

    default:
        return ROS_VECTOR_PASS;     /* Render, IICOp, overlays */
    }
    s->r[4] = 0;                    /* claimed, as GraphicsV drivers say it */
    return ROS_VECTOR_CLAIM;
}

/* ---- the module ----------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    ros_st32(m->private_word, ros_addr(w));

    /* GraphicsV_PixelFormats: NColour, mode flags and log2bpp. These are
     * the formats that the host's decoder knows. */
    static const uint32_t formats[7][3] = {
        { 1, 0, 0 }, { 3, 0, 1 }, { 15, 0, 2 },
        { 255, MF_FULL_PALETTE, 3 },
        { 65535, MF_FULL_PALETTE | MF_RGB, 4 },
        { 0xFFFFFFFFu, 0, 5 },
        { 0xFFFFFFFFu, MF_RGB, 5 },
    };
    memcpy(w->formats, formats, sizeof formats);

    int e = ros_display_init(&w->display);
    if (e) {
        ros_console_printf("DRMVideo: no display (%s): GraphicsV unclaimed\n", strerror(-e));
        return NULL;            /* no hardware: the module stays, idle */
    }
    w->have_display = 1;
    uint32_t sw = w->display.start_width ? w->display.start_width : w->display.width;
    uint32_t sh = w->display.start_height ? w->display.start_height : w->display.height;
    /* Word 4 is the frame rate (-1 "unknown", which is what the Display
     * Manager then shows): the rate the display will scan this mode out at */
    uint32_t sr = w->display.start_rate ? w->display.start_rate : 0xFFFFFFFFu;
    const uint32_t startup[6] = { 1, sw, sh, 5, sr, 0xFFFFFFFFu };
    memcpy(w->startup, startup, sizeof startup);
    return ros_vector_claim_native(ROS_GRAPHICSV, graphicsv, 0);
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    if (w && w->have_display)
        ros_vector_release_native(ROS_GRAPHICSV, graphicsv, 0);
    if (w)
        ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module drmvideo_module = {
    .title = "DRMVideo",
    .help = "DRMVideo\t0.01 (24 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
};
