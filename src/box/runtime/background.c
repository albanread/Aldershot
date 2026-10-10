/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* background.c: the personality lock, the event queue, the background
 * thread and the descriptor pump (background.h).
 *
 * There are three pieces of state, each with its own protection:
 *
 *   - the lock itself: a mutex, with its owner and depth beside it;
 *   - the queue: its own small mutex and condition, so that any thread can
 *     post without the lock;
 *   - the watches: the pump's mutex, and a pipe that wakes the pump when
 *     they change.
 *
 * Everything else here is only touched holding the lock. That covers the
 * IntOff count, the call depth, and whether background work is running.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rosgd/meter.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/platform.h"
#include "rosgd/task.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"

/* ---- the lock ------------------------------------------------------------ */

static pthread_mutex_t lock_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t owner;
static volatile int owned;
static unsigned depth;

static unsigned irq_off;
static int running;                 /* background work in progress */
static pthread_t running_on;
uint32_t ros_call_depth;

int ros_lock_held(void)
{
    return owned && pthread_equal(owner, pthread_self());
}

static void take(unsigned d)
{
    pthread_mutex_lock(&lock_mu);
    owner = pthread_self();
    owned = 1;
    depth = d;
}

static void give(void)
{
    depth = 0;
    owned = 0;
    pthread_mutex_unlock(&lock_mu);
}

void ros_lock(void)
{
    if (ros_lock_held())
        depth++;
    else
        take(1);
}

void ros_unlock(void)
{
    if (depth > 1) {
        depth--;
        return;
    }
    ros_background_run();           /* the releasing thread's safe point */
    give();
}

unsigned ros_blocking_begin(void)
{
    if (!ros_lock_held())
        return 0;                   /* a pump, or a thread outside the runtime */
    unsigned d = depth;
    ros_background_run();
    give();
    return d;
}

void ros_blocking_end(unsigned d)
{
    if (!d)
        return;
    int e = errno;
    take(d);
    ros_background_run();           /* what came while we waited, if not already run */
    errno = e;
}

/* ---- interrupts ---------------------------------------------------------- */

void ros_irq_off(void)
{
    irq_off++;
}

void ros_irq_on(void)
{
    if (irq_off && --irq_off == 0)
        ros_background_run();
}

unsigned ros_irq_off_count(void)
{
    return irq_off;
}

unsigned ros_irq_suspend(void)
{
    unsigned was = irq_off;
    irq_off = 0;
    return was;
}

void ros_irq_resume(unsigned count)
{
    irq_off = count;
}

/* ---- the queue ----------------------------------------------------------- */

struct work {
    struct work *next;
    ros_work_fn *fn;
    void *arg;
    uint32_t info;
};

static pthread_mutex_t queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cv = PTHREAD_COND_INITIALIZER;
static struct work *head, **tail = &head;

/* Whether there is work. This is what compiled code's loops test (ROS_POLL).
 * It is set as work is queued and cleared as the queue empties, both under
 * queue_mu. The test's read is unlocked. A stale read costs a loop one more
 * turn at most. */
volatile int ros_work_pending;

int ros_post(ros_work_fn *fn, void *arg, uint32_t info)
{
    struct work *w = malloc(sizeof *w);
    if (!w)
        return -1;
    *w = (struct work){ NULL, fn, arg, info };
    pthread_mutex_lock(&queue_mu);
    *tail = w;
    tail = &w->next;
    ros_work_pending = 1;
    pthread_cond_signal(&queue_cv);
    pthread_mutex_unlock(&queue_mu);
    return 0;
}

static struct work *take_work(void)
{
    pthread_mutex_lock(&queue_mu);
    struct work *w = head;
    if (w) {
        head = w->next;
        if (!head)
            tail = &head;
    }
    if (!head)
        ros_work_pending = 0;
    pthread_mutex_unlock(&queue_mu);
    return w;
}

/* A compiled loop's back-edge (--poll-loops), with work queued. This is the
 * interrupt that a loop which never calls the OS would otherwise never let
 * in, because the lock is the running task's while it computes.
 * Background work runs, as an IRQ's handlers would. It runs on the SVC
 * stack below the loop's own sp if the loop is on that stack, because the
 * loop's frames are live below where the SWI it is inside began. Otherwise
 * it runs where the SVC stack stands.
 * Only background work runs here. Callbacks wait for a SWI's way out, where
 * nothing native is half done. The CallBack handler would also want
 * registers that lifted code keeps in C locals and not in s. */
void ros_safe_point(struct ros_cpu *s, uint32_t sp)
{
    (void)s;
    if (ros_in_background())
        return;
    uint32_t outer = ros_svc_sp;
    if (sp > ROS_SVCSTACK_BASE && sp <= outer)
        ros_svc_sp = (sp - 16) & ~7u;
    ros_background_run();
    ros_svc_sp = outer;
}

int ros_in_background(void)
{
    return running && pthread_equal(running_on, pthread_self());
}

void ros_background_run(void)
{
    if (!ros_lock_held() || irq_off || running)
        return;
    running = 1;
    running_on = pthread_self();
    uint32_t sema = ros_ld32(ROS_ZP_IRQSEMA);
    ros_st32(ROS_ZP_IRQSEMA, 1);    /* compiled code asks: are we in the background? */
    /* Background work draws on the real display, whatever task it
     * interrupts. Its VDU context is made live, once there is work. */
    void *vdu_was = NULL;
    int vdu_swapped = 0;
    for (struct work *w; (w = take_work());) {
        if (ros_vdu_displays && !vdu_swapped) {
            vdu_was = ros_vdu_background_enter();
            vdu_swapped = 1;
        }
        /* An error has no caller to go back to. Report it, as a callback's
         * is reported, and go on. */
        struct ros_handler h;
        if (ROS_TRY(&h)) {
            w->fn(w->arg, w->info);
            ros_handler_pop(&h);
        } else {
            ros_console_printf("rosgd: background work: %s\n", h.error->errmess);
        }
        free(w);
        if (irq_off)
            break;                  /* the work turned interrupts off: the rest waits */
    }
    if (vdu_swapped)
        ros_vdu_background_leave(vdu_was);
    ros_st32(ROS_ZP_IRQSEMA, sema);
    running = 0;
}

void ros_idle(unsigned ms)
{
    if (!ros_work_pending && ros_lock_held() && !ros_in_background()) {
        unsigned d = ros_blocking_begin();
        pthread_mutex_lock(&queue_mu);
        if (!head) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += (long)ms * 1000000L;
            t.tv_sec += t.tv_nsec / 1000000000L;
            t.tv_nsec %= 1000000000L;
            uint64_t i0 = ros_meter_now_ns();     /* the wait is the box's idle */
            pthread_cond_timedwait(&queue_cv, &queue_mu, &t);
            ros_meter_add_idle(ros_meter_now_ns() - i0);
        }
        pthread_mutex_unlock(&queue_mu);
        ros_blocking_end(d);
    }
    ros_background_run();
}

void ros_swi_exit_outermost(void)
{
    static int in_callbacks;
    ros_background_run();
    if (!in_callbacks && !running) {
        in_callbacks = 1;
        ros_callbacks_run();
        in_callbacks = 0;
    }
}

/* The background thread. Whenever there is work and the lock is free, it
 * takes the lock and runs the work. This is the interrupt that arrives
 * while a task computes, or while a SWI waits. */
static void *background(void *unused)
{
    ros_thread_name("background");      /* named for /proc: what uses the time */
    (void)unused;
    ros_thread_signal_stack();
    for (;;) {
        pthread_mutex_lock(&queue_mu);
        while (!head)
            pthread_cond_wait(&queue_cv, &queue_mu);
        pthread_mutex_unlock(&queue_mu);
        ros_lock();
        ros_background_run();
        int deferred = irq_off && head;
        ros_unlock();
        if (deferred) {
            /* Interrupts are off, and whoever turned them off holds the
             * lock or will run the work at IntOn. Look again shortly. */
            struct timespec ts = { 0, 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* ---- the pump ------------------------------------------------------------ */

#define MAX_WATCHES 256

static struct {
    int fd;
    short events;
    int armed;
    ros_work_fn *fn;
    void *arg;
} watches[MAX_WATCHES];
static int nwatches;
static pthread_mutex_t watch_mu = PTHREAD_MUTEX_INITIALIZER;
static int wake[2] = { -1, -1 };

static void poke(void)
{
    if (wake[1] >= 0) {
        char c = 1;
        (void)!write(wake[1], &c, 1);
    }
}

int ros_watch(int fd, short events, ros_work_fn *fn, void *arg)
{
    pthread_mutex_lock(&watch_mu);
    int i;
    for (i = 0; i < nwatches && watches[i].fd != fd; i++)
        ;
    if (i == nwatches) {
        if (nwatches == MAX_WATCHES) {
            pthread_mutex_unlock(&watch_mu);
            return -1;
        }
        nwatches++;
    }
    watches[i].fd = fd;
    watches[i].events = events;
    watches[i].fn = fn;
    watches[i].arg = arg;
    watches[i].armed = 1;
    pthread_mutex_unlock(&watch_mu);
    poke();
    return 0;
}

void ros_unwatch(int fd)
{
    pthread_mutex_lock(&watch_mu);
    for (int i = 0; i < nwatches; i++)
        if (watches[i].fd == fd) {
            watches[i] = watches[--nwatches];
            break;
        }
    pthread_mutex_unlock(&watch_mu);
    poke();
}

static void *pump(void *unused)
{
    ros_thread_name("bg-pump");      /* named for /proc: what uses the time */
    (void)unused;
    ros_thread_signal_stack();
    struct pollfd p[MAX_WATCHES + 1];
    int which[MAX_WATCHES + 1];
    for (;;) {
        int n = 0;
        p[n++] = (struct pollfd){ .fd = wake[0], .events = POLLIN };
        pthread_mutex_lock(&watch_mu);
        for (int i = 0; i < nwatches; i++)
            if (watches[i].armed) {
                which[n] = i;
                p[n++] = (struct pollfd){ .fd = watches[i].fd, .events = watches[i].events };
            }
        pthread_mutex_unlock(&watch_mu);
        if (poll(p, (nfds_t)n, -1) < 0)
            continue;
        if (p[0].revents) {
            char buf[64];
            (void)!read(wake[0], buf, sizeof buf);
        }
        pthread_mutex_lock(&watch_mu);
        for (int k = 1; k < n; k++) {
            if (!p[k].revents)
                continue;
            int i = which[k];
            /* The watch may have gone or moved while we polled. */
            if (i >= nwatches || watches[i].fd != p[k].fd || !watches[i].armed)
                continue;
            watches[i].armed = 0;
            ros_post(watches[i].fn, watches[i].arg, (uint32_t)(uint16_t)p[k].revents);
        }
        pthread_mutex_unlock(&watch_mu);
    }
    return NULL;
}

int ros_background_start(void)
{
    if (pipe(wake) < 0)
        return -errno;
    fcntl(wake[0], F_SETFL, O_NONBLOCK);
    fcntl(wake[1], F_SETFL, O_NONBLOCK);
    fcntl(wake[0], F_SETFD, FD_CLOEXEC);
    fcntl(wake[1], F_SETFD, FD_CLOEXEC);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* Background work runs compiled code on this thread's C stack. */
    pthread_attr_setstacksize(&attr, 1u << 20);
    int e = pthread_create(&t, &attr, background, NULL);
    if (!e)
        e = pthread_create(&t, &attr, pump, NULL);
    pthread_attr_destroy(&attr);
    return -e;
}
