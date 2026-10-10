/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* sleep.c -- a command's wait, as a RISC OS program waits.
 *
 * On RISC OS a program that waits, for a socket or for time to pass,
 * sleeps on UpCall 6 (UpCall_Sleep) with a poll word. In a task window,
 * TaskWindow claims the upcall and polls the Wimp until the word is
 * non-zero, so the desktop goes on. Outside a task window nobody claims
 * it, and the program waits there as it can. A program in user mode is
 * also pre-empted by TaskWindow. A native command in ROSGD is never
 * pre-empted, so it must sleep. ROS_BLOCKING alone lets background work
 * run during the wait but not the Wimp, so a task window's desktop would
 * stop until the command ended.
 *
 * ros_sleep_fd() sleeps in this way. The poll word is set when the
 * descriptor becomes ready (a watch on the pump, background.c) or when the
 * time runs out (a ticker event). TaskWindow's UpCall_Sleep
 * (modules/taskwindow) waits for the word with the desktop running. If
 * nothing claims the upcall, the wait is a blocking poll() with the lock
 * released.
 *
 * The words are in the RMA, one slot each, and are kept for good. A wake
 * can come after its sleep has ended, for example when a watch's work was
 * already queued or a tick had already run. It then finds its slot given
 * to another sleep (a new serial) and does nothing. At worst it ends that
 * sleep early, which its caller takes as "nothing yet" because it is in a
 * loop.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/ticker.h"

#define UPCALL_SLEEP 6u
#define SLOTS        32

static struct {
    uint32_t word;              /* the poll word's address, 0 until made */
    uint32_t serial;            /* this sleep's, never 0 */
    int busy;
} slots[SLOTS];
static uint32_t serials;
static uint32_t timer_entry;

static void wake_serial(uint32_t serial)
{
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].busy && slots[i].serial == serial)
            ros_st32(slots[i].word, 1);
}

/* The pump saw the descriptor ready (background work, lock held) */
static void on_ready(void *arg, uint32_t revents)
{
    (void)revents;
    wake_serial((uint32_t)(uintptr_t)arg);
}

/* The ticker event: R12 the serial */
static void on_time(struct ros_cpu *s)
{
    wake_serial(s->r[12]);
    s->r[15] = s->r[14];                        /* returned, as a ticker event's code must */
}

/* Blocking: there is no task window to sleep in */
static int block(int fd, short events, unsigned ms)
{
    int r;
    if (fd < 0) {
        struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
        ROS_BLOCKING(nanosleep(&ts, NULL));
        return 0;
    }
    struct pollfd p = { .fd = fd, .events = events };
    ROS_BLOCKING(r = poll(&p, 1, (int)ms));
    return r > 0;
}

int ros_sleep_fd(int fd, short events, unsigned ms, os_error **err)
{
    *err = NULL;
    if (fd >= 0) {
        struct pollfd p = { .fd = fd, .events = events };
        if (poll(&p, 1, 0) > 0)
            return 1;
    }
    if (!ms)
        return 0;
    if (ros_in_background())
        return block(fd, events, ms);

    int slot = -1;
    for (int i = 0; i < SLOTS && slot < 0; i++)
        if (!slots[i].busy)
            slot = i;
    if (slot >= 0 && !slots[slot].word) {
        void *w = ros_rma_alloc(4);
        slots[slot].word = w ? ros_addr(w) : 0;
    }
    if (!timer_entry)
        timer_entry = ros_native_entry(on_time, "rosgd:SleepTimer");
    if (slot < 0 || !slots[slot].word || !timer_entry)
        return block(fd, events, ms);           /* no word to sleep on */

    uint32_t serial = ++serials ? serials : ++serials;
    slots[slot].serial = serial, slots[slot].busy = 1;
    ros_st32(slots[slot].word, 0);
    uint32_t cs = (ms + 9) / 10;                /* the event comes after n + 1 ticks */
    if (ros_ticker_add(cs > 1 ? cs - 1 : 1, 0, timer_entry, serial)) {
        slots[slot].busy = 0;
        return block(fd, events, ms);
    }
    if (fd >= 0)
        ros_watch(fd, events, on_ready, (void *)(uintptr_t)serial);

    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = UPCALL_SLEEP, s.r[1] = slots[slot].word;
    ros_swi(&s, XOS_UpCall);
    int claimed = !s.v && s.r[0] == 0;
    os_error *e = s.v ? ros_ptr(s.r[0]) : NULL;

    if (fd >= 0)
        ros_unwatch(fd);
    ros_ticker_remove(timer_entry, serial);
    slots[slot].busy = 0;

    if (e) {
        *err = e;
        return -1;
    }
    if (!claimed)
        return block(fd, events, ms);           /* not in a task window */
    if (fd < 0)
        return 0;
    struct pollfd p = { .fd = fd, .events = events };
    return poll(&p, 1, 0) > 0;
}
