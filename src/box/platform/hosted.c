/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* hosted.c -- the platform layer's display and input for a hosted build:
 * the runtime and compiled modules as an ordinary macOS or Linux process,
 * for host tests.
 *
 * The display is memory: screen memory at ROS_SCREEN_BASE, with the screen
 * block after it, as the DRM display lays it out, but nothing scans it
 * out. DRMVideo and the VDU drivers above it run as they do in the box.
 * A test reads the pixels back, or writes them out as a screendump to
 * compare with the 32-bit system's. ROSGD_HOSTED_DISPLAY=WxH sets the
 * start-up mode (default 1280x800, the box's), and =none gives no display.
 * There is no input. */
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "rosgd/arena.h"
#include "rosgd/platform.h"
#include "rosgd/screen.h"

#define MAX_WIDTH 1920u
#define MAX_HEIGHT 1200u

static struct ros_screen_block *block;
static uint32_t generation;
static int present;

int ros_display_init(struct ros_display *d)
{
    memset(d, 0, sizeof *d);
    const char *want = getenv("ROSGD_HOSTED_DISPLAY");
    uint32_t w = 1280, h = 800;
    if (want && !strcmp(want, "none"))
        return -ENODEV;
    if (want)
        sscanf(want, "%ux%u", &w, &h);
    if (mmap(ros_ptr(ROS_SCREEN_BASE), ROS_SCREEN_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
        return -errno;
    present = 1;
    d->max_width = MAX_WIDTH;
    d->max_height = MAX_HEIGHT;
    /* the mode list: the standard sizes a 1920 x 1200 host display offers */
    static const struct ros_display_mode list[] = {
        { 640, 400, 60 }, { 640, 480, 60 }, { 800, 600, 60 }, { 1024, 768, 60 },
        { 1152, 864, 60 }, { 1280, 800, 60 }, { 1280, 960, 60 }, { 1280, 1024, 60 },
        { 1400, 1050, 60 }, { 1440, 900, 60 }, { 1600, 1200, 60 }, { 1680, 1050, 60 },
        { 1920, 1080, 60 }, { 1920, 1200, 60 },
    };
    d->nmodes = sizeof list / sizeof list[0];
    memcpy(d->modes, list, sizeof list);
    snprintf(d->name, sizeof d->name, "memory");
    return ros_display_set_mode(d, w, h, 32, 0, 0, 0);
}

int ros_display_set_mode(struct ros_display *d, uint32_t width, uint32_t height,
                         uint32_t bpp, uint32_t pixo, uint32_t stride, uint32_t rows)
{
    if (!present)
        return -ENODEV;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 32)
        return -EINVAL;
    uint32_t natural = ((width * bpp + 31) / 32) * 4;
    if (stride == 0)
        stride = natural;
    if (stride < natural)
        return -EINVAL;
    if (width == 0 || height == 0 || width > MAX_WIDTH || height > MAX_HEIGHT)
        return -ERANGE;
    /* As big as the DRM display makes it: a 32 bpp buffer of the mode's
     * size, so a shallower mode has room to pan (SetDMAAddress), or the
     * rows asked for if that is more */
    if (rows < height)
        rows = height;
    uint64_t size = (uint64_t)stride * rows;
    if (size < (uint64_t)width * 4 * height - ROS_SCREEN_BLOCK)
        size = (uint64_t)width * 4 * height - ROS_SCREEN_BLOCK;
    size = (size + 4095) & ~4095ull;
    if (size + ROS_SCREEN_BLOCK > ROS_SCREEN_SIZE)
        return -EFBIG;
    memset(ros_ptr(ROS_SCREEN_BASE), 0, (size_t)size + ROS_SCREEN_BLOCK);
    block = ros_ptr(ROS_SCREEN_BASE + (uint32_t)size);
    block->version = ROS_SCREEN_VERSION;
    block->generation = ++generation;
    block->xres = width;
    block->yres = height;
    block->pitch = stride;
    block->bpp = bpp;
    block->pixo = pixo ? 1 : 0;
    block->xoffset = block->yoffset = 0;
    atomic_thread_fence(memory_order_release);
    block->magic = ROS_SCREEN_MAGIC;
    d->width = width;
    d->height = height;
    d->stride = stride;
    d->bpp = bpp;
    d->pixo = block->pixo;
    d->base = ROS_SCREEN_BASE;
    d->size = (uint32_t)size;
    return 0;
}

void ros_display_set_palette(struct ros_display *d, uint32_t first, uint32_t n,
                             const uint32_t *bgr)
{
    (void)d;
    if (!block)
        return;
    for (uint32_t i = 0; i < n && first + i < 256; i++)
        block->palette[first + i] = bgr[i] & 0x00FFFFFFu;
}

int ros_display_set_origin(struct ros_display *d, uint32_t addr)
{
    if (!block || addr < d->base)
        return -EINVAL;
    uint32_t off = addr - d->base;
    uint32_t y = off / d->stride, xbits = (off % d->stride) * 8;
    if (xbits % d->bpp || (uint64_t)(y + d->height) * d->stride > d->size)
        return -EINVAL;
    block->xoffset = xbits / d->bpp;
    block->yoffset = y;
    return 0;
}

struct ros_screen_ext *ros_display_ext(void)
{
    return NULL;
}

int ros_display_set_pointer(int how, const uint32_t *image, int32_t x, int32_t y,
                            int32_t hot_x, int32_t hot_y)
{
    (void)how, (void)image, (void)x, (void)y, (void)hot_x, (void)hot_y;
    return present ? 0 : -ENODEV;
}

void ros_display_update(const struct ros_display *d)
{
    (void)d;
}

void ros_display_vsync(void)
{
}

/* The host's network is the host's: nothing to bring up. */
int ros_net_init(void)
{
    return 0;
}

int ros_input_init(void)
{
    return 0;
}

int ros_input_start(void (*deliver)(const struct ros_input_event *ev))
{
    (void)deliver;
    return 0;
}

const char *ros_input_device_name(uint32_t device)
{
    (void)device;
    return "";
}

int ros_input_key_number(unsigned linux_code)
{
    (void)linux_code;
    return -1;
}

const char *ros_display_driver(int *direct)
{
    if (direct)
        *direct = 0;
    return "none (hosted)";
}

/* ---- audio: the null sink (platform/audio_alsa.c has the real one) ------
 *
 * A host with its own sound defines ROS_HOSTED_OWN_AUDIO and supplies
 * every ros_audio_* call of platform.h itself. BBC BASIC V for Mac does
 * this with a CoreAudio sink, which runs the same fill chain on
 * CoreAudio's thread. Without it (every build of the box's own) this is
 * the null sink. */
#ifndef ROS_HOSTED_OWN_AUDIO

static pthread_t audio_thread;
static int audio_running;
#define AUDIO_FILL_MAX 4
static ros_audio_fill *audio_fills[AUDIO_FILL_MAX];
static unsigned audio_nfills;

void ros_audio_start(ros_audio_fill *fn)
{
    audio_fills[0] = fn;
    audio_nfills = fn ? 1 : 0;
}

void ros_audio_add(ros_audio_fill *fn)
{
    if (fn && audio_nfills < AUDIO_FILL_MAX)
        audio_fills[audio_nfills++] = fn;
}

void ros_audio_remove(ros_audio_fill *fn)
{
    for (unsigned i = 0; i < audio_nfills; i++) {
        if (audio_fills[i] != fn)
            continue;
        for (unsigned j = i + 1; j < audio_nfills; j++)
            audio_fills[j - 1] = audio_fills[j];
        audio_nfills--;
        break;
    }
}

int ros_audio_rate(void)
{
    return 44100;
}

static atomic_int audio_muted;

void ros_audio_mute(int on)
{
    atomic_store(&audio_muted, on != 0);
}

int ros_audio_muted(void)
{
    return atomic_load(&audio_muted);
}

static void *audio_pump(void *arg)
{
    (void)arg;
    int16_t *sink = malloc(441 * 2 * 2 * 4);
    while (audio_running) {
        if (sink) {
            memset(sink, 0, 441 * 2 * 4);
            for (unsigned i = 0; i < audio_nfills; i++)
                audio_fills[i](sink, 441);     /* 10 ms, into nothing */
        }
        usleep(10000);
    }
    free(sink);
    return NULL;
}

int ros_audio_init(void)
{
    audio_running = 1;
    return pthread_create(&audio_thread, NULL, audio_pump, NULL);
}

/* A hosted box plays into nothing, so it has no outputs to list or choose
 * between: *ListSpeakers says so, and *SelectSpeaker has nothing to pick */
unsigned ros_audio_outputs(struct ros_audio_output *out, unsigned max)
{
    (void)out, (void)max;
    return 0;
}

int ros_audio_capture(unsigned seconds, const char *path)
{
    (void)seconds, (void)path;
    return -1;                                  /* a hosted box plays into nothing */
}

void ros_audio_select(unsigned card, unsigned device)
{
    (void)card, (void)device;
}

int ros_audio_select_state(void)
{
    return ROS_AUDIO_SELECT_REFUSED;
}

void ros_audio_final(void)
{
    if (!audio_running)
        return;
    audio_running = 0;
    pthread_join(audio_thread, NULL);
}

#endif /* ROS_HOSTED_OWN_AUDIO */
