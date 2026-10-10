/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* display_drm.c -- screen memory from a DRM dumb buffer.
 *
 * The kernel's KMS interface, used directly (no libdrm): find a connected
 * connector and a CRTC that can drive it, and remember them. A mode is a
 * dumb buffer mapped into the arena at ROS_SCREEN_BASE and scanned out.
 * From then on screen memory is ordinary arena memory, which compiled VDU
 * code writes as it wrote the framebuffer on real hardware (the platform
 * layer, platform.h).
 *
 * The buffer holds the mode's own pixel format, whatever DRM was told, and
 * its last 4 KB is the screen block that describes it (screen.h). DRM is
 * told XRGB8888 because it is all Linux's virtio-gpu driver accepts. The
 * host's Metal window reads the block instead. So a buffer is made big
 * enough for the mode at any depth plus the block: width x height at 32 bpp,
 * with rows added for the block.
 *
 * A display whose driver scans a buffer of ours out (nouveau on an NVIDIA
 * card, bochs on QEMU's standard adapter, amdgpu, i915) is direct too
 * whenever the RISC OS mode is 32 bpp &xRGB, which is XRGB8888 itself. The
 * mode is a real mode set on the CRTC, screen memory is the scanned-out
 * buffer, nothing is copied, and the pointer is the hardware cursor. At
 * any other depth that display is converted, because what it scans out is
 * XRGB8888 and the mode is not.
 *
 * A display that only shows what it is given, such as VMware's SVGA adapter
 * (vmwgfx) or a PC's firmware framebuffer (simpledrm), shows its buffer as
 * the XRGB8888 it was told, and only what it is told has changed (DIRTYFB).
 * For those the display is converted. Screen memory is ordinary shared
 * memory in the arena, block and all. A thread of this file's own turns it
 * into XRGB8888 in a buffer the display's own size, fifty times a second.
 * It converts the rows that changed, scaled up by a whole number and
 * centred, with the pointer drawn over them, and tells DRM which rows those
 * were. The display keeps its one mode (as a Pi's HDMI does when the GPU
 * scales), and a RISC OS mode can be any size up to it. rosgd.display=direct
 * or =convert on the command line overrides the choice, which is by the
 * driver: virtio_gpu is direct, and the rest are converted.
 *
 * With no display at all, because there is no DRM device or no connected
 * connector (rosgd-vz with neither --gui nor --gpu), screen memory is still
 * there, as a headless RISC OS machine's is. It is shared memory in the
 * arena, block and all, as converted, with nothing scanning it out (#42).
 * Modes go up to 1920 x 1200 at any depth, with 1280 x 800 to start
 * (hosted.c's memory display). The VDU drivers, the Wimp and the harness's
 * screen reads work as with a display, and nobody sees it.
 *
 * On the HAL (BOX on QEMU without Linux) there is no DRM. The HAL's own
 * call HAL_SYS_SCREEN gives a memfd over QEMU's ramfb, an XRGB8888
 * framebuffer that QEMU shows as it is, and the display is converted into
 * it as into any display's own buffer, with nothing to tell when it
 * changes. Linux answers the call -ENOSYS, so the same /init runs on both.
 * With virtio-gpu in place of ramfb the HAL's screen is any size. Each
 * mode asks for one of its own size (so QEMU's window follows the mode),
 * and the converter says which rows it wrote (HAL_SYS_FLUSH), since
 * virtio-gpu shows only what it is told has changed.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "rosgd/arena.h"
#include "rosgd/platform.h"
#include "rosgd/screen.h"

#define POINTER_SIZE 64         /* virtio-gpu's cursors are 64 x 64 */

static int card = -1;
static int dirtyfb;                     /* rosgd.dirtyfb: copy the screen to the host at 5 Hz */
static unsigned vsyncs;
static uint32_t conn_id, crtc_id;
static struct drm_mode_modeinfo *modes;     /* the connector's */
static uint32_t nmodes;

static struct {
    uint32_t handle, fb_id;
    uint64_t size;              /* the whole buffer, block included */
    struct ros_screen_block *block;
    uint32_t generation;
    /* scanned out (direct): its geometry and mode, and the framebuffers
     * made over it at other rows (screen banks and teletext's flash),
     * shown by a page flip (ros_display_set_origin) */
    uint32_t width, height, pitch;
    struct drm_mode_modeinfo mode;
    uint32_t shown_fb;
    struct { uint32_t y, fb_id; } pan[8];
    uint32_t npan;
} cur;

static int xioctl(int fd, unsigned long req, void *arg);

/* The framebuffers over other rows of the buffer, gone with it */
static void drop_pans(void)
{
    for (uint32_t i = 0; i < cur.npan; i++)
        xioctl(card, DRM_IOCTL_MODE_RMFB, &cur.pan[i].fb_id);
    cur.npan = 0;
    cur.shown_fb = 0;
}

static struct {
    uint32_t handle;
    uint32_t *image;            /* mapped, POINTER_SIZE squared words */
    int shown;
} ptr;

/* Converting: the display's own buffer, and the pointer drawn into it */
static int converting;
static int scanout;             /* the driver scans a buffer of ours out */
static int host_reads_block;    /* virtio-gpu: the host reads the screen block, so any depth */
static struct drm_mode_modeinfo pref_mode;      /* the display's own mode, for converting */
static char driver[32];         /* the DRM driver's name, for the report */
static struct {
    uint32_t handle, fb_id, width, height, pitch;
    uint32_t *px;               /* mapped in the process, not the arena */
} scan;
static int shadow_fd = -1;      /* screen memory: a memfd */
/* rosgd.display=compositor: there is no DRM here. Screen memory is shared
 * with ROSGD's compositor (ports/compositor), a Linux program that owns
 * the display and shows this screen as its bottom layer. The memfd is
 * published as /dev/rosgd-screen, a link to /proc/<pid>/fd/<n> (made anew
 * each mode), and the pointer goes in the block, with its image in
 * POINTER_BYTES before it. */
static int compositor;
#define POINTER_BYTES (POINTER_SIZE * POINTER_SIZE * 4)
static int memory;              /* no display: screen memory is only memory */
static int hal_screen;          /* the HAL's ramfb or virtio-gpu: converted, without DRM */
static int hal_gpu;             /* virtio-gpu's: a screen each mode's size, flushed */
static int hal_fd = -1;
#define HAL_SYS_SCREEN 0x7200
#define HAL_SYS_FLUSH 0x7202
static int hal_resize(uint32_t width, uint32_t height);
static pthread_mutex_t conv_mu = PTHREAD_MUTEX_INITIALIZER;     /* the mapping, cur */
static pthread_mutex_t soft_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t image[POINTER_SIZE * POINTER_SIZE];
    int32_t x, y;
    int shown;
    uint32_t shape;             /* moves on each new image */
} soft;

/* The kernel's names for connector types (drm_connector_enum_list). */
static const char *const connector_types[] = {
    "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS",
    "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI",
    "DPI", "Writeback", "SPI", "USB",
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r < 0 ? -errno : 0;
}

/* A connector's modes and encoders.  The kernel copies modes only when
 * the buffer holds all of them, so ask for the counts first. */
struct connector {
    struct drm_mode_get_connector c;
    struct drm_mode_modeinfo *modes;
    uint32_t *encoders;
};

static int get_connector(uint32_t id, struct connector *out)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        memset(out, 0, sizeof *out);
        out->c.connector_id = id;
        int e = xioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &out->c);  /* probes */
        if (e)
            return e;
        uint32_t nm = out->c.count_modes, nenc = out->c.count_encoders;
        out->modes = calloc(nm + 1, sizeof *out->modes);
        out->encoders = calloc(nenc + 1, sizeof *out->encoders);
        memset(&out->c, 0, sizeof out->c);
        out->c.connector_id = id;
        out->c.count_modes = nm;
        out->c.modes_ptr = (uintptr_t)out->modes;
        out->c.count_encoders = nenc;
        out->c.encoders_ptr = (uintptr_t)out->encoders;
        e = xioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &out->c);
        if (e == 0 && out->c.count_modes == nm && out->c.count_encoders == nenc)
            return 0;
        free(out->modes);
        free(out->encoders);
        if (e)
            return e;
        /* the counts changed under us (a hotplug): ask again */
    }
    return -EAGAIN;
}

static uint32_t crtc_for(const struct connector *con, const uint32_t *crtcs, uint32_t ncrtcs)
{
    struct drm_mode_get_encoder enc;
    if (con->c.encoder_id) {
        memset(&enc, 0, sizeof enc);
        enc.encoder_id = con->c.encoder_id;
        if (xioctl(card, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
            return enc.crtc_id;
    }
    for (uint32_t i = 0; i < con->c.count_encoders; i++) {
        memset(&enc, 0, sizeof enc);
        enc.encoder_id = con->encoders[i];
        if (xioctl(card, DRM_IOCTL_MODE_GETENCODER, &enc))
            continue;
        for (uint32_t j = 0; j < ncrtcs && j < 32; j++)
            if (enc.possible_crtcs & (1u << j))
                return crtcs[j];
    }
    return 0;
}

/* The timings for a size: the connector's own mode if it has one, else
 * made up.  virtio-gpu has no timings to honour, only a size, and accepts
 * any size up to the host display's. */
static int mode_for(uint32_t w, uint32_t h, struct drm_mode_modeinfo *m)
{
    for (uint32_t i = 0; i < nmodes; i++)
        if (modes[i].hdisplay == w && modes[i].vdisplay == h) {
            *m = modes[i];
            return 0;
        }
    if (w < 1 || h < 1 || w > 16384 || h > 16384)
        return -ERANGE;
    memset(m, 0, sizeof *m);
    m->hdisplay = (uint16_t)w;
    m->hsync_start = (uint16_t)(w + 8);
    m->hsync_end = (uint16_t)(w + 40);
    m->htotal = (uint16_t)(w + 80);
    m->vdisplay = (uint16_t)h;
    m->vsync_start = (uint16_t)(h + 3);
    m->vsync_end = (uint16_t)(h + 9);
    m->vtotal = (uint16_t)(h + 30);
    m->vrefresh = 60;
    m->clock = (uint32_t)((uint64_t)m->htotal * m->vtotal * 60 / 1000);
    m->type = DRM_MODE_TYPE_USERDEF;
    snprintf(m->name, sizeof m->name, "%ux%u", w, h);
    return 0;
}

static void destroy_buffer(uint32_t handle, uint32_t fb_id)
{
    if (fb_id)
        xioctl(card, DRM_IOCTL_MODE_RMFB, &fb_id);
    if (handle) {
        struct drm_mode_destroy_dumb d = { .handle = handle };
        xioctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
}

/* Screen memory for a mode, the direct way: a dumb buffer, scanned out */
static int direct_buffer(uint32_t width, uint32_t height, uint64_t need, uint64_t *size)
{
    struct drm_mode_modeinfo mode;
    int e = mode_for(width, height, &mode);
    if (e)
        return e;
    /* width x rows at 32 bpp: room for the mode's rows at its own stride,
     * and the block after them. */
    uint32_t rows = (uint32_t)((need + (uint64_t)width * 4 - 1) / ((uint64_t)width * 4));
    if (rows < height)
        rows = height;
    struct drm_mode_create_dumb dumb = { .width = width, .height = rows, .bpp = 32 };
    if ((e = xioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)))
        return e;
    if (dumb.size > ROS_SCREEN_SIZE || dumb.size < need) {
        destroy_buffer(dumb.handle, 0);
        return -EFBIG;
    }
    struct drm_mode_fb_cmd fb = { .width = width, .height = height, .pitch = dumb.pitch,
                                  .bpp = 32, .depth = 24, .handle = dumb.handle };
    if ((e = xioctl(card, DRM_IOCTL_MODE_ADDFB, &fb))) {
        destroy_buffer(dumb.handle, 0);
        return e;
    }
    struct drm_mode_map_dumb map = { .handle = dumb.handle };
    if ((e = xioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map))) {
        destroy_buffer(dumb.handle, fb.fb_id);
        return e;
    }

    /* Scan the new buffer out before touching the arena, so a refusal
     * leaves the old mode whole. */
    struct drm_mode_crtc set = { .set_connectors_ptr = (uintptr_t)&conn_id,
                                 .count_connectors = 1, .crtc_id = crtc_id,
                                 .fb_id = fb.fb_id, .mode_valid = 1, .mode = mode };
    if ((e = xioctl(card, DRM_IOCTL_MODE_SETCRTC, &set))) {
        destroy_buffer(dumb.handle, fb.fb_id);
        return e == -EINVAL ? -ERANGE : e;
    }

    /* Screen memory is the new buffer from here.  The whole screen region
     * goes back to reserved first, so a smaller mode leaves nothing of the
     * old buffer mapped past its end. */
    mmap(ros_ptr(ROS_SCREEN_BASE), ROS_SCREEN_SIZE, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if ((e = ros_arena_map_fd(ROS_SCREEN_BASE, (size_t)dumb.size, card, map.offset)))
        return e;       /* not recoverable: the arena lost its screen */
    drop_pans();
    destroy_buffer(cur.handle, cur.fb_id);
    memset(ros_ptr(ROS_SCREEN_BASE), 0, (size_t)dumb.size);
    cur.handle = dumb.handle;
    cur.fb_id = fb.fb_id;
    cur.width = width, cur.height = height, cur.pitch = dumb.pitch, cur.mode = mode;
    cur.shown_fb = fb.fb_id;
    *size = dumb.size;
    return 0;
}

/* Screen memory for a mode, converted: shared memory, a new memfd each
 * mode (so it starts clear); the display's own mode stays as it is.  Called
 * holding conv_mu, which keeps the converter off the old mapping. */
static int shared_buffer(uint64_t need, uint64_t *size)
{
    uint64_t bytes = (need + 4095) & ~(uint64_t)4095;
    if (bytes > ROS_SCREEN_SIZE)
        return -EFBIG;
    int fd = memfd_create("rosgd-screen", MFD_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (ftruncate(fd, (off_t)bytes) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    mmap(ros_ptr(ROS_SCREEN_BASE), ROS_SCREEN_SIZE, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    int e = ros_arena_map_fd(ROS_SCREEN_BASE, (size_t)bytes, fd, 0);
    if (e) {
        close(fd);
        return e;
    }
    if (shadow_fd >= 0)
        close(shadow_fd);
    shadow_fd = fd;
    *size = bytes;
    if (compositor) {
        char to[64];
        snprintf(to, sizeof to, "/proc/%d/fd/%d", (int)getpid(), fd);
        unlink("/dev/rosgd-screen");
        if (symlink(to, "/dev/rosgd-screen") != 0)
            ros_console_printf("rosgd: /dev/rosgd-screen: %s\n", strerror(errno));
    }
    return 0;
}

static int converted_buffer(uint32_t width, uint32_t height, uint64_t need, uint64_t *size)
{
    if (width > scan.width || height > scan.height)
        return -ERANGE;
    return shared_buffer(need, size);
}

static int start_converting(const struct drm_mode_modeinfo *mode);

/* Give up the display's own buffer and let the converter idle: the mode
 * that follows is scanned out directly.  Under conv_mu, which the
 * converter holds while it writes. */
static void stop_converting(void)
{
    if (!converting)
        return;
    converting = 0;
    if (scan.px)
        munmap(scan.px, (size_t)scan.pitch * scan.height);
    destroy_buffer(scan.handle, scan.fb_id);
    memset(&scan, 0, sizeof scan);
}

/* Whether this mode can be scanned out as it is. The driver must take a
 * buffer of ours, and what it scans out is XRGB8888, which only a 32 bpp
 * &xRGB mode is. The exception is virtio-gpu, where the host reads the
 * screen block and decodes every depth itself. */
static int direct_mode(uint32_t bpp, uint32_t pixo)
{
    return scanout && (host_reads_block || (bpp == 32 && !pixo));
}

int ros_display_set_mode(struct ros_display *d, uint32_t width, uint32_t height,
                         uint32_t bpp, uint32_t pixo, uint32_t stride, uint32_t frows)
{
    if (card < 0 && !memory && !hal_screen)
        return -ENODEV;
    if (memory && (width > d->max_width || height > d->max_height))
        return -ERANGE;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 32)
        return -EINVAL;
    uint32_t natural = ((width * bpp + 31) / 32) * 4;
    if (stride == 0)
        stride = natural;
    if (stride < natural)
        return -EINVAL;
    if (width < 1 || height < 1)
        return -ERANGE;

    /* room for the mode's rows at its own stride (or the rows asked for,
     * if more: teletext's two banks), and the block after them */
    if (frows < height)
        frows = height;
    uint64_t need = (uint64_t)stride * frows + ROS_SCREEN_BLOCK, size = 0;
    if (compositor)
        need += POINTER_BYTES;          /* the pointer's image, before the block */
    int e, locked = 0;
    /* Which way this mode is shown.  A display that scans our buffer out
     * takes a 32 bpp mode as it is; every other mode, and every other
     * display, is converted.  Either can follow the other, so a box can be
     * in a converted 8 bpp mode one moment and a scanned-out desktop the
     * next. */
    int want_direct = !memory && direct_mode(bpp, pixo);
    if (memory) {
        e = shared_buffer(need, &size);
    } else {
        pthread_mutex_lock(&conv_mu);
        locked = 1;
        if (want_direct) {
            /* Try the mode before giving up the converted one: a mode the
             * display refuses must leave the old one whole. */
            int was = converting;
            converting = 0;
            e = direct_buffer(width, height, need, &size);
            if (!e) {
                converting = was;
                stop_converting();      /* the display's own buffer, if there was one */
            } else {
                converting = was;
                if (was) {              /* a mode it cannot set is converted instead */
                    want_direct = 0;
                    e = converted_buffer(width, height, need, &size);
                }
            }
        } else if (converting) {
            e = hal_gpu ? hal_resize(width, height) : 0;    /* the HAL's screen: the mode's size */
            if (!e)
                e = converted_buffer(width, height, need, &size);
        } else {
            /* back from a scanned-out mode: the display takes its own mode
             * again, and the converter the screen */
            if ((e = start_converting(&pref_mode)) == 0) {
                converting = 1;
                if ((e = converted_buffer(width, height, need, &size)) == 0) {
                    drop_pans();
                    destroy_buffer(cur.handle, cur.fb_id);
                    cur.handle = cur.fb_id = 0;
                }
            }
        }
    }
    if (e) {
        if (locked)
            pthread_mutex_unlock(&conv_mu);
        return e;
    }

    cur.size = size;
    cur.block = ros_ptr(ROS_SCREEN_BASE + (uint32_t)size - ROS_SCREEN_BLOCK);
    struct ros_screen_block *b = cur.block;
    b->version = ROS_SCREEN_VERSION;
    b->generation = ++cur.generation;
    b->xres = width;
    b->yres = height;
    b->pitch = stride;
    b->bpp = bpp;
    b->pixo = pixo ? 1 : 0;
    b->xoffset = b->yoffset = 0;
    b->ptr_x = b->ptr_y = 0, b->ptr_shown = 0, b->ptr_shape = 0;
    b->ptr_image = compositor ? (uint32_t)(size - ROS_SCREEN_BLOCK - POINTER_BYTES) : 0;
    memset((uint8_t *)b + ROS_SCREEN_EXT, 0, sizeof(struct ros_screen_ext));
    atomic_thread_fence(memory_order_release);
    b->magic = ROS_SCREEN_MAGIC;        /* last: the host reads a whole block */
    if (locked)
        pthread_mutex_unlock(&conv_mu);

    d->width = width;
    d->height = height;
    d->stride = stride;
    d->bpp = bpp;
    d->pixo = b->pixo;
    d->base = ROS_SCREEN_BASE;
    d->size = (uint32_t)size - ROS_SCREEN_BLOCK - (compositor ? POINTER_BYTES : 0);
    if (dirtyfb)                        /* how to read QEMU's copy (tests/lib/deskdrive.py) */
        ros_console_printf("rosgd: screen: %ux%u, %u bpp, stride %u, %s\n", width, height,
                           bpp, stride, bpp == 32 ? (b->pixo ? "xBGR" : "xRGB") : "packed");
    return 0;
}

void ros_display_set_palette(struct ros_display *d, uint32_t first, uint32_t n,
                             const uint32_t *bgr)
{
    (void)d;
    if (!cur.block)
        return;
    for (uint32_t i = 0; i < n && first + i < 256; i++)
        cur.block->palette[first + i] = bgr[i] & 0x00FFFFFFu;
}

#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258u        /* 'XR24' */
#endif

/* Scan out the buffer from row y: its framebuffer (made the first time,
 * kept until the buffer goes), then a page flip */
static void flip_to(uint32_t y)
{
    uint32_t fb = cur.fb_id;
    if (y) {
        fb = 0;
        for (uint32_t i = 0; i < cur.npan && !fb; i++)
            if (cur.pan[i].y == y)
                fb = cur.pan[i].fb_id;
        if (!fb) {
            if (cur.npan == sizeof cur.pan / sizeof cur.pan[0])
                return;                             /* more origins than a mode has banks */
            struct drm_mode_fb_cmd2 f = { .width = cur.width, .height = cur.height,
                                          .pixel_format = DRM_FORMAT_XRGB8888 };
            f.handles[0] = cur.handle;
            f.pitches[0] = cur.pitch;
            f.offsets[0] = y * cur.pitch;
            if (xioctl(card, DRM_IOCTL_MODE_ADDFB2, &f))
                return;
            cur.pan[cur.npan].y = y;
            cur.pan[cur.npan++].fb_id = f.fb_id;
            fb = f.fb_id;
        }
    }
    if (fb == cur.shown_fb)
        return;
    struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc_id, .fb_id = fb };
    if (xioctl(card, DRM_IOCTL_MODE_PAGE_FLIP, &pf)) {
        struct drm_mode_crtc set = { .set_connectors_ptr = (uintptr_t)&conn_id,
                                     .count_connectors = 1, .crtc_id = crtc_id,
                                     .fb_id = fb, .mode_valid = 1, .mode = cur.mode };
        if (xioctl(card, DRM_IOCTL_MODE_SETCRTC, &set))
            return;
    }
    cur.shown_fb = fb;
}

int ros_display_set_origin(struct ros_display *d, uint32_t addr)
{
    if (!cur.block || addr < d->base)
        return -EINVAL;
    uint32_t off = addr - d->base;
    uint32_t y = off / d->stride, xbits = (off % d->stride) * 8;
    if (xbits % d->bpp || (uint64_t)(y + d->height) * d->stride > d->size)
        return -EINVAL;
    cur.block->xoffset = xbits / d->bpp;
    cur.block->yoffset = y;
    /* A buffer scanned out as it is (32 bpp at the dumb buffer's own
     * pitch) shows the new rows by a framebuffer over them and a page
     * flip, at the next vertical blank (or, a flip still pending, by
     * setting the CRTC at once). Every other way of showing the screen
     * reads the block's offsets: the converter, and the hosts' windows. */
    if (!memory && !converting && cur.fb_id && d->bpp == 32 && d->stride == cur.pitch && !xbits)
        flip_to(y);
    return 0;
}

struct ros_screen_ext *ros_display_ext(void)
{
    return compositor && cur.block ? (struct ros_screen_ext *)((uint8_t *)cur.block + ROS_SCREEN_EXT) : NULL;
}

int ros_display_set_pointer(int how, const uint32_t *image, int32_t x, int32_t y,
                            int32_t hot_x, int32_t hot_y)
{
    if (compositor && cur.block) {
        /* for the compositor: the image first, then where and whether */
        struct ros_screen_block *b = cur.block;
        if (image && (how == ROS_POINTER_SHAPE || !b->ptr_shown) && b->ptr_image) {
            memcpy(ros_ptr(ROS_SCREEN_BASE + b->ptr_image), image, POINTER_BYTES);
            b->ptr_hot = (uint32_t)(hot_x & 0xFFFF) | (uint32_t)(hot_y & 0xFFFF) << 16;
            atomic_thread_fence(memory_order_release);
            b->ptr_shape++;
        }
        b->ptr_x = x, b->ptr_y = y;
        b->ptr_shown = how != ROS_POINTER_HIDE;
        return 0;
    }
    if (memory)
        return 0;                       /* nothing shows it */
    if (card < 0 && !hal_screen)
        return -ENODEV;
    if (converting) {
        pthread_mutex_lock(&soft_mu);
        if (how == ROS_POINTER_HIDE) {
            soft.shown = 0;
        } else {
            if (image && (how == ROS_POINTER_SHAPE || !soft.shown)) {
                memcpy(soft.image, image, sizeof soft.image);
                soft.shape++;
            }
            soft.shown = 1;
        }
        soft.x = x;
        soft.y = y;
        pthread_mutex_unlock(&soft_mu);
        return 0;
    }
    int e;
    if (!ptr.handle) {
        struct drm_mode_create_dumb dumb = { .width = POINTER_SIZE, .height = POINTER_SIZE,
                                             .bpp = 32 };
        if ((e = xioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)))
            return e;
        struct drm_mode_map_dumb map = { .handle = dumb.handle };
        if ((e = xioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map)))
            return e;
        void *p = mmap(NULL, (size_t)dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, card,
                       (off_t)map.offset);
        if (p == MAP_FAILED)
            return -errno;
        ptr.handle = dumb.handle;
        ptr.image = p;
    }
    /* CURSOR2: the image's hot spot goes with it, the cursor plane's
     * HOTSPOT_X/Y, which virtio-gpu passes to the host */
    struct drm_mode_cursor2 c = { .crtc_id = crtc_id, .x = x, .y = y };
    if (how == ROS_POINTER_HIDE) {
        c.flags = DRM_MODE_CURSOR_BO;           /* handle 0: off */
        ptr.shown = 0;
    } else if (how == ROS_POINTER_SHAPE || !ptr.shown) {
        if (image)
            memcpy(ptr.image, image, POINTER_SIZE * POINTER_SIZE * 4);
        c.flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE;
        c.handle = ptr.handle;
        c.width = c.height = POINTER_SIZE;
        c.hot_x = hot_x, c.hot_y = hot_y;
        ptr.shown = 1;
    } else {
        c.flags = DRM_MODE_CURSOR_MOVE;
    }
    return xioctl(card, DRM_IOCTL_MODE_CURSOR2, &c);
}

/* ---- converting ------------------------------------------------------------ */

static uint32_t rgb565[65536];          /* as the Metal window decodes 16 bpp */

/* One row of the mode's pixels, as XRGB8888 */
static void convert_row(uint32_t *out, const uint8_t *src, uint32_t n, uint32_t bpp,
                        uint32_t pixo, const uint32_t *pal)
{
    switch (bpp) {
    case 32:
        for (uint32_t x = 0; x < n; x++) {
            uint32_t w;
            memcpy(&w, src + 4 * x, 4);
            out[x] = pixo ? (w & 0xFF) << 16 | (w & 0xFF00) | (w >> 16 & 0xFF) : w & 0xFFFFFF;
        }
        break;
    case 16:
        for (uint32_t x = 0; x < n; x++)
            out[x] = rgb565[src[2 * x] | src[2 * x + 1] << 8];
        break;
    default: {                          /* palettised, the leftmost pixel lowest */
        uint32_t mask = (1u << bpp) - 1;
        for (uint32_t x = 0; x < n; x++)
            out[x] = pal[(src[x * bpp / 8] >> (x * bpp & 7)) & mask];
    }
    }
}

/* Rows [lo, hi) must be converted again: the pointer was or is there */
struct span { int32_t lo, hi; };

static int in(const struct span *s, int32_t y)
{
    return y >= s->lo && y < s->hi;
}

static void *converter(void *unused)
{
    ros_thread_name("display");      /* named for /proc: what uses the time */
    (void)unused;
    for (uint32_t w = 0; w < 65536; w++)
        rgb565[w] = ((w >> 11 & 31) * 255 / 31) << 16 | ((w >> 5 & 63) * 255 / 63) << 8 |
                    (w & 31) * 255 / 31;
    uint32_t *line = malloc(16384 * sizeof *line);
    uint8_t *prev = NULL;               /* the rows as last converted */
    size_t prev_size = 0;
    uint32_t gen = 0, xo = 0, yo = 0, pal[256] = { 0 }, raw_pal[256] = { 0 };
    int32_t drawn_x = 0, drawn_y = 0;
    int drawn = 0;
    uint32_t drawn_shape = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    /* Power: a still screen is looked at less often. After IDLE_AFTER
     * frames with nothing changed (a tenth of a second), the whole screen
     * is compared only every IDLE_EVERY-th frame (10 a second). The
     * pointer is still looked at every frame, which costs nothing. A
     * change seen puts the converter back at fifty a second for as long as
     * changes keep coming, as in a window dragged or an animation, while a
     * clock that moves once a second costs one fast frame. Reading the
     * whole screen fifty times a second was most of what an idle box cost
     * a laptop. */
    enum { IDLE_AFTER = 5, IDLE_EVERY = 5 };
    unsigned still = 0, tick = 0;
    for (;;) {
        next.tv_nsec += 20000000;       /* fifty frames a second */
        if (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        struct timespec woke;
        clock_gettime(CLOCK_MONOTONIC, &woke);
        if (woke.tv_sec > next.tv_sec + 1)
            next = woke;                /* far behind (the VM stopped): no catching up */

        pthread_mutex_lock(&conv_mu);
        struct ros_screen_block *b = cur.block;
        /* idle while the screen is scanned out as it is (direct_mode):
         * there is no display buffer of ours to convert into */
        if (!converting || !scan.px) {
            pthread_mutex_unlock(&conv_mu);
            continue;
        }
        if (!b || b->magic != ROS_SCREEN_MAGIC || !line) {
            pthread_mutex_unlock(&conv_mu);
            continue;
        }
        uint32_t xres = b->xres, yres = b->yres, pitch = b->pitch, bpp = b->bpp;
        uint32_t pixo = b->pixo, xoff = b->xoffset, yoff = b->yoffset;
        uint32_t ncol = bpp <= 8 ? 1u << bpp : 0;
        int full = b->generation != gen || xoff != xo || yoff != yo ||
                   (ncol && memcmp(raw_pal, b->palette, ncol * 4));
        tick++;
        size_t rowbytes = ((size_t)xres * bpp + 7) / 8;
        if (xres == 0 || yres == 0 || xres > 16384 || (!full && rowbytes * yres > prev_size)) {
            pthread_mutex_unlock(&conv_mu);
            continue;
        }
        if (full) {
            gen = b->generation, xo = xoff, yo = yoff;
            memcpy(raw_pal, b->palette, sizeof raw_pal);
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = raw_pal[i];
                pal[i] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
            }
            if (rowbytes * yres > prev_size) {
                free(prev);
                prev_size = rowbytes * yres;
                prev = malloc(prev_size);
                if (!prev) {
                    prev_size = 0;
                    pthread_mutex_unlock(&conv_mu);
                    continue;
                }
            }
            memset(scan.px, 0, (size_t)scan.pitch * scan.height);
        }
        uint32_t s = scan.width / xres < scan.height / yres ? scan.width / xres
                                                           : scan.height / yres;
        if (s < 1)
            s = 1;
        uint32_t ox = scan.width > xres * s ? (scan.width - xres * s) / 2 : 0;
        uint32_t oy = scan.height > yres * s ? (scan.height - yres * s) / 2 : 0;
        uint32_t wout = xres * s < scan.width ? xres * s : scan.width;
        const uint8_t *src0 = (const uint8_t *)ros_ptr(ROS_SCREEN_BASE) + (size_t)yoff * pitch +
                              (size_t)xoff * bpp / 8;

        /* the pointer: where it was drawn and where it is now */
        pthread_mutex_lock(&soft_mu);
        int shown = soft.shown;
        int32_t px = soft.x, py = soft.y;
        int moved = shown != drawn || px != drawn_x || py != drawn_y || soft.shape != drawn_shape;
        /* idle: only the pointer's rows, unless it is a frame to look */
        int look = full || still < IDLE_AFTER || tick % IDLE_EVERY == 0;
        struct span old = { drawn ? drawn_y : 0, drawn ? drawn_y + POINTER_SIZE : 0 };
        struct span now = { shown ? py : 0, shown ? py + POINTER_SIZE : 0 };
        if (!moved)
            old.lo = old.hi = now.lo = now.hi = 0;
        static uint32_t image[POINTER_SIZE * POINTER_SIZE];
        memcpy(image, soft.image, sizeof image);
        drawn = shown, drawn_x = px, drawn_y = py, drawn_shape = soft.shape;
        pthread_mutex_unlock(&soft_mu);

        int32_t lo = (int32_t)yres, hi = -1;
        int content = 0;
        for (uint32_t y = 0; y < yres; y++) {
            const uint8_t *src = src0 + (size_t)y * pitch;
            uint8_t *p = prev + y * rowbytes;
            int ptr_row = in(&old, (int32_t)y) || in(&now, (int32_t)y);
            if (!full && !ptr_row && (!look || memcmp(p, src, rowbytes) == 0))
                continue;
            if (!ptr_row || memcmp(p, src, rowbytes) != 0)
                content = 1;
            memcpy(p, src, rowbytes);
            convert_row(line, p, xres, bpp, pixo, pal);
            if (shown && (int32_t)y >= py && (int32_t)y < py + POINTER_SIZE) {
                const uint32_t *pr = image + ((int32_t)y - py) * POINTER_SIZE;
                for (int32_t i = 0; i < POINTER_SIZE; i++) {
                    int32_t x = px + i;
                    if (x >= 0 && x < (int32_t)xres && pr[i] >> 24 >= 0x80)
                        line[x] = pr[i] & 0xFFFFFF;
                }
            }
            for (uint32_t dy = 0; dy < s; dy++) {
                uint32_t row = oy + y * s + dy;
                if (row >= scan.height)
                    break;
                uint32_t *out = scan.px + (size_t)row * (scan.pitch / 4) + ox;
                if (s == 1)
                    memcpy(out, line, wout * 4);
                else
                    for (uint32_t x = 0; x < wout; x++)
                        out[x] = line[x / s];
            }
            if ((int32_t)y < lo)
                lo = (int32_t)y;
            hi = (int32_t)y;
        }
        pthread_mutex_unlock(&conv_mu);
        /* still: a frame looked at in which nothing but the pointer
         * changed; any change of the screen's own starts the count again */
        if (full || content)
            still = 0;
        else if (look)
            still++;

        if (hi >= lo) {
            struct drm_clip_rect clip = { 0, 0, (uint16_t)scan.width, (uint16_t)scan.height };
            if (!full) {
                clip.x1 = (uint16_t)ox;
                clip.x2 = (uint16_t)(ox + wout);
                clip.y1 = (uint16_t)(oy + (uint32_t)lo * s);
                clip.y2 = (uint16_t)(oy + ((uint32_t)hi + 1) * s < scan.height
                                         ? oy + ((uint32_t)hi + 1) * s : scan.height);
            }
            struct drm_mode_fb_dirty_cmd dirty = { .fb_id = scan.fb_id, .num_clips = 1,
                                                   .clips_ptr = (uintptr_t)&clip };
            if (card >= 0)              /* (ramfb is read as it is) */
                xioctl(card, DRM_IOCTL_MODE_DIRTYFB, &dirty);
            else if (hal_gpu)
                syscall(HAL_SYS_FLUSH, clip.x1, clip.y1, clip.x2 - clip.x1, clip.y2 - clip.y1);
        }
    }
    return NULL;
}

/* The converter's thread: one, however often this is called */
static int start_converter(void)
{
    static int running;
    if (!running) {
        pthread_t t;
        int e = pthread_create(&t, NULL, converter, NULL);
        if (e)
            return -e;
        pthread_detach(t);
        running = 1;
    }
    return 0;
}

/* The display's own buffer, at its own mode, scanned out once and kept */
static int start_converting(const struct drm_mode_modeinfo *mode)
{
    struct drm_mode_create_dumb dumb = { .width = mode->hdisplay, .height = mode->vdisplay,
                                         .bpp = 32 };
    int e = xioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    if (e)
        return e;
    struct drm_mode_fb_cmd fb = { .width = mode->hdisplay, .height = mode->vdisplay,
                                  .pitch = dumb.pitch, .bpp = 32, .depth = 24,
                                  .handle = dumb.handle };
    if ((e = xioctl(card, DRM_IOCTL_MODE_ADDFB, &fb))) {
        destroy_buffer(dumb.handle, 0);
        return e;
    }
    struct drm_mode_map_dumb map = { .handle = dumb.handle };
    if ((e = xioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map))) {
        destroy_buffer(dumb.handle, fb.fb_id);
        return e;
    }
    void *px = mmap(NULL, (size_t)dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, card,
                    (off_t)map.offset);
    if (px == MAP_FAILED) {
        e = -errno;
        destroy_buffer(dumb.handle, fb.fb_id);
        return e;
    }
    memset(px, 0, (size_t)dumb.size);
    struct drm_mode_crtc set = { .set_connectors_ptr = (uintptr_t)&conn_id,
                                 .count_connectors = 1, .crtc_id = crtc_id,
                                 .fb_id = fb.fb_id, .mode_valid = 1, .mode = *mode };
    if ((e = xioctl(card, DRM_IOCTL_MODE_SETCRTC, &set))) {
        munmap(px, (size_t)dumb.size);
        destroy_buffer(dumb.handle, fb.fb_id);
        return e;
    }
    scan.handle = dumb.handle;
    scan.fb_id = fb.fb_id;
    scan.width = mode->hdisplay;
    scan.height = mode->vdisplay;
    scan.pitch = dumb.pitch;
    scan.px = px;
    return start_converter();
}

/* What this driver can do with a buffer of ours: scan it out, or only show
 * what it is given. The kernel offers no way to ask, so this goes by name.
 * The drivers with a display engine of their own scan out. A driver whose
 * plane copies into a framebuffer somewhere else (simpledrm's firmware
 * one, vmwgfx's host surface) does not. virtio-gpu scans out too, and its
 * host reads the screen block, so every depth is direct there rather than
 * 32 bpp alone. rosgd.display=direct or =convert settles it instead. */
static void probe_scanout(void)
{
    static const char *const scanners[] = { "nouveau", "bochs-drm", "amdgpu", "radeon",
                                            "i915", "xe", "nvidia-drm" };
    struct drm_version v = { .name_len = sizeof driver - 1, .name = driver };
    if (xioctl(card, DRM_IOCTL_VERSION, &v) == 0) {
        host_reads_block = strcmp(driver, "virtio_gpu") == 0;
        scanout = host_reads_block;
        for (size_t i = 0; !scanout && i < sizeof scanners / sizeof scanners[0]; i++)
            scanout = strcmp(driver, scanners[i]) == 0;
    }
    const char *how = ros_cmdline_value("rosgd.display");
    if (how && strcmp(how, "direct") == 0)
        scanout = 1;
    else if (how && strcmp(how, "convert") == 0)
        scanout = host_reads_block = 0;
}

/* No display: screen memory all the same, as hosted.c's memory display */
static int memory_display(struct ros_display *d)
{
    static const struct ros_display_mode list[] = {
        { 640, 400, 60 }, { 640, 480, 60 }, { 800, 600, 60 }, { 1024, 768, 60 },
        { 1152, 864, 60 }, { 1280, 800, 60 }, { 1280, 960, 60 }, { 1280, 1024, 60 },
        { 1400, 1050, 60 }, { 1440, 900, 60 }, { 1600, 1200, 60 }, { 1680, 1050, 60 },
        { 1920, 1080, 60 }, { 1920, 1200, 60 },
    };
    if (card >= 0)
        close(card);
    card = -1;
    free(modes);
    modes = NULL, nmodes = 0;
    memset(d, 0, sizeof *d);
    memory = 1;
    d->max_width = 1920;
    d->max_height = 1200;
    d->nmodes = sizeof list / sizeof list[0];
    memcpy(d->modes, list, sizeof list);
    snprintf(d->name, sizeof d->name, compositor ? "the compositor's" : "none (memory)");
    return ros_display_set_mode(d, 1280, 800, 32, 0, 0, 0);
}

static int drm_display(struct ros_display *d);

/* The HAL's virtio-gpu screen made width x height: the old one unmapped
 * first (the HAL frees it), the new one mapped.  Under conv_mu. */
static int hal_resize(uint32_t width, uint32_t height)
{
    if (scan.width == width && scan.height == height && scan.px)
        return 0;
    struct { int32_t fd; uint32_t width, height, pitch, gpu; } hs;
    if (scan.px)
        munmap(scan.px, (size_t)scan.pitch * scan.height);
    scan.px = NULL;
    if (hal_fd >= 0)
        close(hal_fd);
    hal_fd = -1;
    if (syscall(HAL_SYS_SCREEN, &hs, width, height) != 0)
        return -errno;
    void *px = mmap(NULL, (size_t)hs.pitch * hs.height, PROT_READ | PROT_WRITE, MAP_SHARED, hs.fd, 0);
    if (px == MAP_FAILED)
        return -errno;
    hal_fd = hs.fd;
    scan.width = hs.width, scan.height = hs.height, scan.pitch = hs.pitch, scan.px = px;
    return 0;
}

/* The HAL's screen: ramfb's framebuffer or virtio-gpu's, converted into.
 * It is -ENODEV under Linux, which does not know the call. */
static int hal_display(struct ros_display *d)
{
    static const struct ros_display_mode list[] = {
        { 640, 400, 60 }, { 640, 480, 60 }, { 800, 600, 60 }, { 1024, 768, 60 },
        { 1152, 864, 60 }, { 1280, 800, 60 }, { 1280, 960, 60 }, { 1280, 1024, 60 },
        { 1400, 1050, 60 }, { 1440, 900, 60 }, { 1600, 1200, 60 }, { 1680, 1050, 60 },
        { 1920, 1080, 60 }, { 1920, 1200, 60 },
    };
    struct { int32_t fd; uint32_t width, height, pitch, gpu; } hs;
    if (syscall(HAL_SYS_SCREEN, &hs, 0, 0) != 0)
        return -ENODEV;
    hal_gpu = hs.gpu != 0;
    hal_fd = hs.fd;
    size_t bytes = (size_t)hs.pitch * hs.height;
    void *px = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, hs.fd, 0);
    if (px == MAP_FAILED)
        return -errno;
    memset(d, 0, sizeof *d);
    scan.width = hs.width, scan.height = hs.height, scan.pitch = hs.pitch, scan.px = px;
    hal_screen = converting = 1;
    snprintf(driver, sizeof driver, hal_gpu ? "virtio-gpu" : "ramfb");
    snprintf(d->name, sizeof d->name, hal_gpu ? "virtio-gpu" : "ramfb");
    d->max_width = hal_gpu ? 1920 : hs.width;           /* virtio-gpu: any mode, each its own size */
    d->max_height = hal_gpu ? 1200 : hs.height;
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++)
        if (list[i].width <= d->max_width && list[i].height <= d->max_height)
            d->modes[d->nmodes++] = list[i];
    /* ramfb's own size, if the list has not: its largest mode */
    if (!hal_gpu && (!d->nmodes || d->modes[d->nmodes - 1].width != hs.width ||
                     d->modes[d->nmodes - 1].height != hs.height))
        d->modes[d->nmodes++] = (struct ros_display_mode){ hs.width, hs.height, 60 };
    d->start_width = hs.width;      /* a window on the Mac: its own size reads well */
    d->start_height = hs.height;
    d->start_rate = 60;
    int e = start_converter();
    if (e)
        return e;
    ros_console_printf("rosgd: %s, every mode converted, at %ux%u\n", driver, hs.width, hs.height);
    return ros_display_set_mode(d, hs.width, hs.height, 32, 0, 0, 0);
}

/* What is driving the screen, for *MachineInfo: the DRM driver's name and
 * whether it scans the box's own screen memory out or is handed a copy */
const char *ros_display_driver(int *direct)
{
    if (direct)
        *direct = scanout;
    return driver[0] ? driver : "none";
}

int ros_display_init(struct ros_display *d)
{
    dirtyfb = ros_cmdline_has("rosgd.dirtyfb");
    const char *how = ros_cmdline_value("rosgd.display");
    if (how && strcmp(how, "compositor") == 0) {
        compositor = 1;                 /* the compositor owns the display */
        return memory_display(d);
    }
    if (hal_display(d) == 0)
        return 0;
    int e = drm_display(d);
    if (e == -ENOENT || e == -ENODEV)   /* no device, or nothing connected */
        e = memory_display(d);
    return e;
}

static int drm_display_card(struct ros_display *d, unsigned n)
{
    char path[32];
    memset(d, 0, sizeof *d);
    snprintf(path, sizeof path, "/dev/dri/card%u", n);
    card = open(path, O_RDWR | O_CLOEXEC);
    if (card < 0)
        return -errno;

    uint32_t crtcs[32], conns[32], encs[32], fbs[32];
    struct drm_mode_card_res res = { 0 };
    int e = xioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res);
    if (e)
        return e;
    res.count_crtcs = res.count_crtcs < 32 ? res.count_crtcs : 32;
    res.count_connectors = res.count_connectors < 32 ? res.count_connectors : 32;
    res.count_encoders = res.count_encoders < 32 ? res.count_encoders : 32;
    res.count_fbs = res.count_fbs < 32 ? res.count_fbs : 32;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.connector_id_ptr = (uintptr_t)conns;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    if ((e = xioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res)))
        return e;
    uint32_t ncrtcs = res.count_crtcs < 32 ? res.count_crtcs : 32;
    uint32_t nconns = res.count_connectors < 32 ? res.count_connectors : 32;

    for (uint32_t i = 0; i < nconns; i++) {
        struct connector con;
        if (get_connector(conns[i], &con))
            continue;
        if (con.c.connection != 1 || con.c.count_modes == 0) {  /* 1: connected */
            free(con.modes);
            free(con.encoders);
            continue;
        }
        struct drm_mode_modeinfo pref = con.modes[0];
        for (uint32_t m = 0; m < con.c.count_modes; m++) {
            if (con.modes[m].type & DRM_MODE_TYPE_PREFERRED)
                pref = con.modes[m];
            if (con.modes[m].hdisplay > d->max_width)
                d->max_width = con.modes[m].hdisplay;
            if (con.modes[m].vdisplay > d->max_height)
                d->max_height = con.modes[m].vdisplay;
        }
        uint32_t crtc = crtc_for(&con, crtcs, ncrtcs);
        free(con.encoders);
        if (!crtc) {
            free(con.modes);
            d->max_width = d->max_height = 0;
            continue;
        }
        conn_id = conns[i];
        crtc_id = crtc;
        modes = con.modes;
        nmodes = con.c.count_modes;
        uint32_t type = con.c.connector_type, type_id = con.c.connector_type_id;
        snprintf(d->name, sizeof d->name, "%s-%u",
                 type < sizeof connector_types / sizeof connector_types[0]
                     ? connector_types[type] : "Unknown",
                 type_id);
        pref_mode = pref;
        probe_scanout();
        if (!scanout) {
            /* the display's mode is the largest a RISC OS mode can be */
            if ((e = start_converting(&pref)))
                return e;
            converting = 1;
            d->max_width = pref.hdisplay;
            d->max_height = pref.vdisplay;
            /* The start-up mode. The display's own is too fine to read on a
             * PC's screen (8-pixel characters at 1920 across), so this is
             * the largest whole scale that leaves at least 640 x 400, as a
             * Pi's GPU scales a low-resolution mode up. rosgd.startwidth=W
             * asks for at least W across instead (the height in the same
             * proportion). The PC image's 1280 gives VMware's own
             * 1280 x 800, and a 4K screen 1920 x 1080. */
            uint32_t min_w = 640, min_h = 400, s = 1;
            const char *sw = ros_cmdline_value("rosgd.startwidth");
            if (sw) {
                uint32_t w = (uint32_t)strtoul(sw, NULL, 10);
                if (w >= 320) {
                    min_w = w;
                    min_h = w * 400 / 640;
                }
            }
            while (pref.hdisplay / (s + 1) >= min_w && pref.vdisplay / (s + 1) >= min_h)
                s++;
            d->start_width = pref.hdisplay / s & ~7u;
            d->start_height = pref.vdisplay / s;
            d->start_rate = pref.vrefresh ? pref.vrefresh : 60;  /* the CRTC still runs pref */
        } else {
            /* Scanned out: there is no scaling to do it with, so the same
             * rule picks a size and the display must have a mode that size.
             * A 4K screen starts at its own 1920 x 1080, which the monitor
             * fills the screen with. Otherwise its own mode. */
            uint32_t min_w = 640, min_h = 400, s = 1;
            const char *sw = ros_cmdline_value("rosgd.startwidth");
            if (sw) {
                uint32_t w = (uint32_t)strtoul(sw, NULL, 10);
                if (w >= 320) {
                    min_w = w;
                    min_h = w * 400 / 640;
                }
            }
            while (pref.hdisplay / (s + 1) >= min_w && pref.vdisplay / (s + 1) >= min_h)
                s++;
            d->start_width = pref.hdisplay;
            d->start_height = pref.vdisplay;
            d->start_rate = pref.vrefresh ? pref.vrefresh : 60;
            for (uint32_t m = 0; s > 1 && m < nmodes; m++)
                if (modes[m].hdisplay == pref.hdisplay / s &&
                    modes[m].vdisplay == pref.vdisplay / s) {
                    d->start_width = modes[m].hdisplay;
                    d->start_height = modes[m].vdisplay;
                    d->start_rate = modes[m].vrefresh ? modes[m].vrefresh : 60;
                    break;
                }
        }
        /* The display's mode list, as a monitor description's: each size
         * and frame rate the connector offers, once each, by size then
         * rate. OS_ScreenMode 2 enumerates over it. A mode that the
         * connector lists as 0 Hz is kept as 60, virtio-gpu's own. */
        for (uint32_t m = 0; m < nmodes && d->nmodes < ROS_DISPLAY_MODES; m++) {
            struct ros_display_mode one = {
                con.modes[m].hdisplay, con.modes[m].vdisplay,
                con.modes[m].vrefresh ? con.modes[m].vrefresh : 60,
            };
            if (one.width > d->max_width || one.height > d->max_height)
                continue;
            unsigned k = 0;
            while (k < d->nmodes &&
                   !(d->modes[k].width == one.width && d->modes[k].height == one.height &&
                     d->modes[k].rate == one.rate))
                k++;
            if (k < d->nmodes)
                continue;
            k = 0;                        /* insert by size, then rate */
            while (k < d->nmodes &&
                   (d->modes[k].width < one.width ||
                    (d->modes[k].width == one.width &&
                     (d->modes[k].height < one.height ||
                      (d->modes[k].height == one.height && d->modes[k].rate < one.rate)))))
                k++;
            memmove(&d->modes[k + 1], &d->modes[k], (d->nmodes - k) * sizeof d->modes[0]);
            d->modes[k] = one;
            d->nmodes++;
        }
        ros_console_printf("rosgd: drm: %s, %s, %s at %ux%u\n", driver[0] ? driver : "?",
                           scanout ? (host_reads_block ? "the host reads the screen block"
                                                       : "32 bpp modes scanned out as they are")
                                   : "every mode converted",
                           d->name, pref.hdisplay, pref.vdisplay);
        return ros_display_set_mode(d, pref.hdisplay, pref.vdisplay, 32, 0, 0, 0);
    }
    return -ENODEV;
}

/* Which card drives the screen. A PC may have more than one, a graphics
 * card and the processor's own, and the monitor is on one of them. So
 * each is tried in turn, and the first with a display connected to it is
 * the one used. rosgd.card=<n> names one instead, for a machine with two
 * screens where the other one is wanted. */
static int drm_display(struct ros_display *d)
{
    const char *want = ros_cmdline_value("rosgd.card");
    for (unsigned n = 0; n < 8; n++) {
        if (want && strtoul(want, NULL, 10) != n)
            continue;
        if (!drm_display_card(d, n))
            return 0;
        stop_converting();                      /* nothing of that card's is kept */
        converting = 0;
        modes = NULL;
        nmodes = conn_id = crtc_id = 0;
        if (card >= 0)
            close(card);
        card = -1;
    }
    return -ENODEV;
}

/* Each VSync: with rosgd.dirtyfb, every tenth one copies the screen to the
 * host, for a display that does not read guest RAM. QEMU's Cocoa window
 * and its screendump are such displays (run/run-x86_64.sh sets it for
 * them). */
void ros_display_vsync(void)
{
    if (dirtyfb && ++vsyncs >= 10) {
        vsyncs = 0;
        ros_display_update(NULL);
    }
}

int32_t ros_display_map_position(uint32_t axis, int32_t fraction)
{
    const struct ros_screen_block *b = cur.block;
    if (!converting || !b || !b->xres || !b->yres)
        return fraction;
    uint32_t sx = scan.width / b->xres, sy = scan.height / b->yres;
    uint32_t s = sx < sy ? sx : sy;
    if (s < 1)
        s = 1;
    /* as the converter places it: the mode's pixels, s to a side, centred */
    int64_t whole = axis == ROS_AXIS_X ? scan.width : scan.height;
    int64_t size = (int64_t)(axis == ROS_AXIS_X ? b->xres : b->yres) * s;
    int64_t origin = whole > size ? (whole - size) / 2 : 0;
    int64_t at = (int64_t)fraction * whole / 65536 - origin;
    int64_t f = at * 65536 / size;
    return (int32_t)(f < 0 ? 0 : f > 65535 ? 65535 : f);
}

void ros_display_update(const struct ros_display *d)
{
    (void)d;
    if (card < 0 || !cur.fb_id)
        return;
    struct drm_mode_fb_dirty_cmd dirty = { .fb_id = cur.fb_id };   /* no clips: all of it */
    xioctl(card, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}
