/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* backing.c -- a window's backing: the buffer its chrome is drawn into
 * when the presenter composites it as a surface (design 37, interim
 * status 5.2, Sprint 3).
 *
 * A backing is a memfd holding a sprite area with one sprite, 32 bpp with
 * an alpha channel, the window's outline in size.  It is published as
 * /dev/rosgd-backing-<id>, a link to this process's descriptor, so that
 * the presenter maps the same pages and draws them as a texture.  The id,
 * the stride and the size go into the window's entry of block v2's
 * surfaces list (screen.h).
 *
 * In the arena a backing is mapped only while it is drawn into (design 37,
 * Sprint 6): the backings window (ROS_BACKING_BASE, 128 MB) holds those
 * drawn most recently, and the least recently drawn goes out when room is
 * wanted.  Every way in is wimp_backing_area -- the VDU's output switched
 * to its sprite (OS_SpriteOp 60, runtime/vdu/sprout.c) so that the Wimp's
 * drawing lands there unchanged, and the Wimp's own passes over its
 * pixels -- which maps it in if it is out.  Between wimp_backing_begin and
 * wimp_backing_end a backing is pinned: the VDU's output is in it, and a
 * nested begin's saved destination may be.  The address may differ each
 * time; nothing keeps it (a sprite output save area holds no address).
 * The presenter's view is its own mapping and is never touched.  So 128
 * windows' backings cost 128 MB of the arena, not one GB. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "rosgd/platform.h"
#include "rosgd/vdu.h"
#include "wimp.h"
#include "../../runtime/vdu/sprite.h"

#define BACKINGS 128                    /* as many as block v2's surfaces list holds */
#define GRAIN (1u << 20)                /* the backings window is given out in 1 MB pieces */
#define GRAINS (ROS_BACKING_SIZE / GRAIN)

static uint8_t grain_used[GRAINS];

/* n pieces of the backings window, together: the first piece's index, or -1 */
static int grains_take(uint32_t n)
{
    for (uint32_t i = 0; i + n <= GRAINS;) {
        uint32_t k = 0;
        while (k < n && !grain_used[i + k])
            k++;
        if (k == n) {
            memset(grain_used + i, 1, n);
            return (int)i;
        }
        i += k + 1;
    }
    return -1;
}

static void grains_give(uint32_t addr, uint32_t size)
{
    uint32_t i = (addr - ROS_BACKING_BASE) / GRAIN, n = (size + GRAIN - 1) / GRAIN;
    if (i + n <= GRAINS)
        memset(grain_used + i, 0, n);
}

static struct {
    int fd;                     /* -1: free */
    uint32_t addr, size;        /* in the arena (0: not mapped now), the memfd's size */
    uint32_t used;              /* when last mapped or asked for (the clock) */
    int pins;                   /* begins without their ends: not to go out */
    uint32_t window;            /* the Wimp window's handle */
    uint32_t save;              /* its VDU save area (384 bytes, the RMA's), for OS_SpriteOp 60 */
    int dirty;                  /* drawn into since the list last gave its entry a frame */
} backings[BACKINGS];

/* The sprite's mode word: RISC OS 5's new format, type 6 (32 bpp) at 90
 * dpi, with the alpha channel flag -- the top byte of each pixel is its
 * opacity, which the Wimp's masked drawing writes and the presenter reads. */
#define MODE_32_ALPHA ((6u << 27) | (90u << 14) | (90u << 1) | 1u | (1u << 15))

static uint32_t clock_now;          /* the backings' uses, counted */

/* Out of the arena: the slot's address space given back, the memfd kept */
static void map_out(int i)
{
    if (!backings[i].addr)
        return;
    ros_arena_unmap(backings[i].addr, backings[i].size);
    grains_give(backings[i].addr, backings[i].size);
    backings[i].addr = 0;
}

/* Into the arena, if it is not: the least recently used unpinned ones go
 * out until there is room.  Returns its address, or 0. */
static uint32_t map_in(int i)
{
    backings[i].used = ++clock_now;
    if (backings[i].addr)
        return backings[i].addr;
    uint32_t n = (backings[i].size + GRAIN - 1) / GRAIN;
    for (;;) {
        int g = grains_take(n);
        if (g >= 0) {
            uint32_t addr = ROS_BACKING_BASE + (uint32_t)g * GRAIN;
            if (ros_arena_map_fd(addr, backings[i].size, backings[i].fd, 0) != 0) {
                grains_give(addr, backings[i].size);
                return 0;
            }
            backings[i].addr = addr;
            return addr;
        }
        int lru = -1;
        for (int k = 0; k < BACKINGS; k++)
            if (k != i && backings[k].fd >= 0 && backings[k].addr && !backings[k].pins &&
                (lru < 0 || backings[k].used < backings[lru].used))
                lru = k;
        if (lru < 0)
            return 0;                       /* all pinned: none to spare */
        map_out(lru);
    }
}

static uint32_t area_bytes(uint32_t w, uint32_t h)
{
    return 16 + 44 + w * 4 * h;
}

/* A backing for a window of w x h pixels: its id (1 up), or 0 */
uint32_t wimp_backing_new(uint32_t window, uint32_t w, uint32_t h, uint32_t *stride)
{
    if (!w || !h || (uint64_t)16 + 44 + (uint64_t)w * 4 * h > ROS_BACKING_SIZE)
        return 0;
    /* the slots free to begin with: before the search, or on the first
     * call every slot's fd of 0 reads as taken and no backing is ever made
     * (so it was on the box until 14 Oct) */
    static int once;
    if (!once) {
        for (int k = 0; k < BACKINGS; k++)
            backings[k].fd = -1;
        once = 1;
    }
    int i;
    for (i = 0; i < BACKINGS && backings[i].fd >= 0; i++)
        ;
    if (i == BACKINGS)
        return 0;
    uint32_t size = (area_bytes(w, h) + 4095u) & ~4095u;
    char name[32];
    snprintf(name, sizeof name, "rosgd-backing-%d", i + 1);
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0)
        return 0;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return 0;
    }
    backings[i].fd = fd;
    backings[i].size = size;
    backings[i].addr = 0;
    backings[i].pins = 0;
    uint32_t addr = map_in(i);
    if (!addr) {                            /* no room even so: this one stays on the screen */
        backings[i].fd = -1;
        close(fd);
        return 0;
    }
    /* the sprite area, and its one sprite: a header of 44 bytes then the
     * rows, each w words, no mask (the alpha is in the pixels) */
    ros_st32(addr + SA_END, size);
    ros_st32(addr + SA_NUMBER, 1);
    ros_st32(addr + SA_FIRST, 16);
    ros_st32(addr + SA_FREE, area_bytes(w, h));
    uint32_t sp = addr + 16;
    ros_st32(sp + SP_NEXT, 44 + w * 4 * h);
    char sname[12] = "backing";
    for (int k = 0; k < 12; k++)
        ros_st8(sp + SP_NAME + (uint32_t)k, (uint8_t)sname[k]);
    ros_st32(sp + SP_WIDTH, w - 1);         /* words a row, less one: 32 bpp */
    ros_st32(sp + SP_HEIGHT, h - 1);
    ros_st32(sp + SP_LBIT, 0);
    ros_st32(sp + SP_RBIT, 31);
    ros_st32(sp + SP_IMAGE, 44);
    ros_st32(sp + SP_TRANS, 44);            /* no mask */
    ros_st32(sp + SP_MODE, MODE_32_ALPHA);
    /* the presenter's way in */
    char to[64], node[64];
    snprintf(to, sizeof to, "/proc/%d/fd/%d", (int)getpid(), fd);
    snprintf(node, sizeof node, "/dev/rosgd-backing-%d", i + 1);
    unlink(node);
    if (symlink(to, node) != 0)
        ros_console_printf("rosgd: %s: %s\n", node, strerror(errno));
    if (!backings[i].save) {
        void *mem;
        if (xos_module_claim(384, &mem) == NULL) {
            backings[i].save = ros_addr(mem);
            ros_st32(backings[i].save, 0);  /* not yet a save area: the RMA's
                                               block is not zeroed, and a stray
                                               tag fails the switch */
        }
    }
    backings[i].window = window;
    if (stride)
        *stride = w * 4;
    return (uint32_t)i + 1;
}

/* Drawing into a backing: the VDU's output switched to its sprite
 * (OS_SpriteOp 60, with a save area of this backing's own, as
 * errorbox.c's saves do) and the graphics origin moved to minus the
 * window's outline's bottom left, so that the Wimp's drawing, which is
 * in screen coordinates, lands in the sprite where it would on the
 * screen.  end() puts both back.  The previous destination comes back
 * in R0-R3 and is replayed to restore it. */
static uint32_t backing_correct;           /* the Wimp's sprout_correct before begin() */

int wimp_backing_begin(uint32_t id, int32_t ox, int32_t oy, uint32_t saved[4])
{
    uint32_t area = wimp_backing_area(id);
    if (!area)
        return 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    /* 60 + &200: R1 the area, R2 a pointer to the sprite.  Plain 60 is
     * the system area with R2 a name, which found nothing and left the
     * output on the screen -- quietly, since begin() returns 0 then (the
     * box, 14 Oct: every backing stayed alpha 0) */
    struct wimp_ws *w = wimp_ws();
    uint32_t correct = w->sprout_correct;
    c.r[0] = 60 + 0x200, c.r[1] = area, c.r[2] = area + 16, c.r[3] = backings[id - 1].save;
    ros_swi(&c, XOS_SpriteOp);
    if (c.v)                                /* the V flag: an error, output unchanged */
        return 0;
    memcpy(saved, c.r, 16);
    backings[id - 1].pins++;                /* in the arena until its end */
    /* The Wimp keeps the screen's mode variables while it draws into a
     * backing: the backing stands for the screen, its drawing is in the
     * screen's coordinates.  Service_SwitchingOutputToSprite has told the
     * Wimp of the switch; left to itself, its next PlotIcon re-read the
     * sizes from the sprite, the window's height became the screen's, and
     * every sprite icon above it was taken to be off the screen and not
     * plotted (the Filer's icons, all but the bottom row, 14 Oct) */
    backing_correct = correct;
    w->sprout_correct = w->sprout_current;
    backings[id - 1].dirty = 1;             /* the list gives the entry a new frame */
    /* the destination's origin, in OS units: the outline's bottom left
     * becomes 0,0 of the sprite.  Not VDU 29: the Wimp's VDU 26 on the
     * way (PlotIcon, copies) zeroed that and the chrome landed outside the
     * sprite (14 Oct) */
    vdu_dest_offset(-ox, -oy, 1);          /* and plots opaque: the sprite has alpha */
    /* the sprite's VDU context, fresh, is VDU 4 and the defaults: a task's
     * PRINT went to the text cursor at the sprite's top left, opaque black
     * behind it (the block over BackMe's title, 14 Oct).  The Wimp's
     * redraw is VDU 5, text at the graphics cursor, as on the screen */
    wimp_vdu5();
    return 1;
}

void wimp_backing_end(const uint32_t saved[4])
{
    struct ros_cpu c;
    struct wimp_ws *w = wimp_ws();
    uint32_t sel, area, sprite;
    ros_vdu_output(&sel, &area, &sprite);   /* the backing being left: unpinned */
    for (int k = 0; k < BACKINGS; k++)
        if (backings[k].fd >= 0 && backings[k].addr == area && area && backings[k].pins) {
            backings[k].pins--;
            break;
        }
    vdu_dest_offset(0, 0, 0);
    /* back to where output was, whose sizes the Wimp still has: the
     * service the switch issues finds nothing to read again */
    w->sprout_correct = backing_correct;
    ros_cpu_enter(&c);
    memcpy(c.r, saved, 16);
    ros_swi(&c, XOS_SpriteOp);
    w->sprout_correct = w->sprout_current;
}

/* Whether the backing was drawn into since this was last asked; cleared */
int wimp_backing_dirty(uint32_t id)
{
    if (!id || id > BACKINGS || backings[id - 1].fd < 0)
        return 0;
    int d = backings[id - 1].dirty;
    backings[id - 1].dirty = 0;
    return d;
}

/* Whether any backing was drawn into since the list was last written */
int wimp_backing_any_dirty(void)
{
    for (int i = 0; i < BACKINGS; i++)
        if (backings[i].fd >= 0 && backings[i].dirty)
            return 1;
    return 0;
}

/* The backing's sprite area in the arena (for OS_SpriteOp 60), or 0 */
uint32_t wimp_backing_area(uint32_t id)
{
    if (!id || id > BACKINGS || backings[id - 1].fd < 0)
        return 0;
    return map_in((int)id - 1);             /* mapped now if it was out */
}

void wimp_backing_free(uint32_t id)
{
    if (!id || id > BACKINGS || backings[id - 1].fd < 0)
        return;
    char node[64];
    snprintf(node, sizeof node, "/dev/rosgd-backing-%u", id);
    unlink(node);
    map_out((int)id - 1);
    backings[id - 1].pins = 0;
    close(backings[id - 1].fd);
    backings[id - 1].fd = -1;
    backings[id - 1].window = 0;
}
