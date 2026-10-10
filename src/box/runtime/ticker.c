/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* ticker.c -- the centisecond tick (ticker.h).
 *
 * The pump is a thread that sleeps to each centisecond boundary of the
 * monotonic clock and records how many centiseconds have passed. It posts
 * one piece of work at a time. That work runs up to the centiseconds that
 * had passed when it was posted. Then, if more have passed since, it posts
 * itself again for the rest. The new work is queued behind whatever was
 * queued meanwhile, which the same safe point runs first. So the ticks
 * still all run at the next safe point, but never ahead of work posted
 * after them. Consider background work that waits, such as a task busy
 * between SWIs. A key's release queued behind the tick comes before the
 * centiseconds after it. The autorepeat (keyboard.c) then does not count
 * time the key was up. Before this, a busy Edit that was starting got
 * Ctrl-F12 three times from one press.
 *
 * The tick's work does what the kernel's timer handler does, once for each
 * centisecond owed. It counts MetroGnome up, calls TickerV, and runs the
 * ticker chain. The chain is the kernel's. It is a list in firing order,
 * with each node holding the ticks still to go after the node before it, so
 * a tick only ever counts down the head. Handlers run with interrupts off,
 * as they did.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/keyboard.h"
#include "rosgd/task.h"
#include "rosgd/ticker.h"
#include "rosgd/vector.h"

#define ERR_BAD_TIME 0x1E5u     /* "Invalid time interval" (hdr/NewErrors) */

static uint64_t base_ns;                /* when MetroGnome was 0 */
static _Atomic uint32_t target;         /* centiseconds passed, by the clock, when
                                           the queued tick was posted */
static atomic_int queued;               /* a tick's work is in the queue */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint32_t ros_monotonic_cs(void)
{
    return base_ns ? (uint32_t)((now_ns() - base_ns) / 10000000u) : 0;
}

/* ---- the chain (lock held) ---------------------------------------------- */

struct node {
    struct node *next;
    uint32_t redo;              /* ticks between calls, or 0: call once */
    uint32_t code, r12;
    uint32_t left;              /* ticks after the node before */
};

static struct node *chain;

static void insert(struct node *n, uint32_t ticks)
{
    struct node **p = &chain;
    /* After every node due at or before it: equal times fire in order. */
    while (*p && (*p)->left <= ticks) {
        ticks -= (*p)->left;
        p = &(*p)->next;
    }
    if (*p)
        (*p)->left -= ticks;
    n->left = ticks;
    n->next = *p;
    *p = n;
}

os_error *ros_ticker_add(uint32_t cs, uint32_t every, uint32_t code, uint32_t r12)
{
    if (cs == 0 || cs >= 0xFFFFFC00u)
        return ros_error(ERR_BAD_TIME, "Invalid time interval");
    struct node *n = malloc(sizeof *n);
    if (!n)
        return ros_error(0x1E3u, "System heap full");
    n->redo = every ? cs + 1 : 0;
    n->code = code;
    n->r12 = r12;
    insert(n, cs + 1);
    return NULL;
}

os_error *ros_ticker_remove(uint32_t code, uint32_t r12)
{
    for (struct node **p = &chain; *p;) {
        struct node *n = *p;
        if (n->code == code && n->r12 == r12) {
            *p = n->next;
            if (*p)
                (*p)->left += n->left;  /* its wait passes to the next */
            free(n);
        } else {
            p = &n->next;
        }
    }
    return NULL;
}

/* Calls a ticker event's code with R12 as its value. It is entered as the
 * kernel entered it, with interrupts off. An error has nobody to go to, so
 * it is reported. */
static void fire(struct node *n)
{
    struct ros_handler h;
    struct ros_cpu s;
    ros_irq_off();
    if (ROS_TRY(&h)) {
        ros_cpu_enter(&s);
        s.r[12] = n->r12;
        ros_call(&s, n->code);
        if (s.r[15] != ROS_RETURN_TO_NATIVE)
            ros_bad_return(&s, ROS_RETURN_TO_NATIVE);
        ros_handler_pop(&h);
    } else {
        ros_console_printf("rosgd: ticker event &%08X: %s\n", n->code, h.error->errmess);
    }
    ros_irq_on();
}

/* ---- the tick ------------------------------------------------------------ */

static void one_tick(int last)
{
    ros_st32(ROS_ZP_METROGNOME, ros_ld32(ROS_ZP_METROGNOME) + 1);
    ros_timers_tick(last);              /* clocks, interval timer, VSync */
    ros_keyboard_tick();                /* autorepeat, INKEY's count */

    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_vector_call(ROS_TICKERV, &s);

    if (!chain)
        return;
    if (chain->left > 1) {
        chain->left--;
        return;
    }
    chain->left = 0;
    while (chain && chain->left == 0) {
        struct node *n = chain;
        chain = n->next;
        fire(n);
        if (n->redo)
            insert(n, n->redo);
        else
            free(n);
    }
}

static void tick_work(void *arg, uint32_t info)
{
    (void)arg, (void)info;
    uint32_t t = atomic_load(&target);   /* this tick's target, read before the pump may post the next */
    atomic_store(&queued, 0);
    int32_t behind;
    while ((behind = (int32_t)(t - ros_ld32(ROS_ZP_METROGNOME))) > 0)
        one_tick(behind == 1);
    /* The centiseconds that passed while this waited. They are posted after
     * whatever was posted meanwhile, unless the pump has queued a tick
     * already. */
    uint32_t passed = ros_monotonic_cs();
    if ((int32_t)(passed - t) > 0 && !atomic_exchange(&queued, 1)) {
        atomic_store(&target, passed);
        ros_post(tick_work, NULL, 0);
    }
}

/* ---- the pump ------------------------------------------------------------- */

static void *pump(void *unused)
{
    ros_thread_name("ticker");      /* named for /proc, to show what uses the time */
    (void)unused;
    ros_thread_signal_stack();
    uint64_t next = now_ns();
    for (;;) {
        next += 10000000u;
        uint64_t now = now_ns();
        if (next > now) {
            uint64_t wait = next - now;
            struct timespec ts = { (time_t)(wait / 1000000000u), (long)(wait % 1000000000u) };
            nanosleep(&ts, NULL);
        } else if (now - next > 1000000000u) {
            next = now;                 /* the host slept, so do not race to catch up */
        }
        uint32_t passed = ros_monotonic_cs();
        if (!atomic_exchange(&queued, 1)) {
            atomic_store(&target, passed);
            ros_post(tick_work, NULL, 0);
        }
    }
    return NULL;
}

int ros_ticker_start(void)
{
    base_ns = now_ns();
    ros_st32(ROS_ZP_METROGNOME, 0);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&t, &attr, pump, NULL);
    pthread_attr_destroy(&attr);
    return -e;
}

/* ---- the SWIs ------------------------------------------------------------ */

os_error *xos_call_after(uint32_t cs, uint32_t code, uint32_t r12)
{
    return ros_ticker_add(cs, 0, code, r12);
}

os_error *xos_call_every(uint32_t cs, uint32_t code, uint32_t r12)
{
    return ros_ticker_add(cs, 1, code, r12);
}

os_error *xos_remove_ticker_event(uint32_t code, uint32_t r12)
{
    return ros_ticker_remove(code, r12);
}
