/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_tasks.c: tasks and the baton, against task.h.
 *
 * /init, task 0, creates two tasks, A and B, each with its own application
 * slot, and passes the baton around them as the Wimp would at Wimp_Poll.
 * Each task leaves marks in what is its own: application space at &8000,
 * the bottom of its SVC stack, and its floating point status. It checks,
 * when the baton comes back, that they are still there. The script:
 *
 *   task 0 -> A: marks, a handler pushed      -> B
 *   B: marks, raises and catches its own error -> A
 *   A: checks; waits (blocking), when B must not run but ticks must;
 *      2000 switches with B, timed as they fall, then (on Linux and
 *      the HAL, given three cores) both pinned to one core, then each to
 *      its own, which gives the cost of a switch that stays and of one that
 *      moves; tells B to stop -> B
 *   B: checks, returns: the baton goes to its creator, task 0
 *   task 0: checks its own state -> A, which returns -> task 0
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/platform.h"
#include "rosgd/task.h"
#include "selftest.h"

#define check ros_check

#define MARK_AT (ROS_SVCSTACK_GUARD_AT + ROS_SVCSTACK_GUARD + 16)    /* below any frame, above the guard */

static struct ros_task *A, *B;
static pthread_t thread_a, thread_b;

static struct {
    uint32_t domain_a, domain_b;
    int a_kept, b_kept;             /* marks still there when the baton came back */
    int b_ran_while_a_waited;
    uint32_t ticks_while_waiting;
    int b_caught_own, a_handler_intact;
    double switch_us, same_us, cross_us;   /* unpinned; both on one core; each on its own */
    int same_core, core_a, core_b;
    volatile int stop;
    unsigned b_pings, pings_timed;  /* B's rounds; those of the first, unpinned, timing */
    int b_done, a_done;
} r;

static uint32_t mg(void)
{
    return ros_ld32(ROS_ZP_METROGNOME);
}

static void marks(uint32_t app, uint32_t svc, uint32_t fpsr)
{
    ros_st32(ROS_APP_BASE, app);
    ros_st32(ROS_APP_BASE + 0xFFFC, app ^ 0xFFFFFFFFu);
    ros_st32(MARK_AT, svc);
    ros_fp_current->fpsr = fpsr;
}

static int marks_kept(uint32_t app, uint32_t svc, uint32_t fpsr)
{
    return ros_ld32(ROS_APP_BASE) == app && ros_ld32(ROS_APP_BASE + 0xFFFC) == (app ^ 0xFFFFFFFFu) &&
           ros_ld32(MARK_AT) == svc && ros_fp_current->fpsr == fpsr;
}

/* 2000 switches, A to B and back: the time of one */
static double pingpong(void)
{
    struct timespec s0, s1;
    clock_gettime(CLOCK_MONOTONIC, &s0);
    for (int i = 0; i < 1000; i++)
        ros_task_switch(B);
    clock_gettime(CLOCK_MONOTONIC, &s1);
    return ((double)(s1.tv_sec - s0.tv_sec) * 1e6 + (double)(s1.tv_nsec - s0.tv_nsec) / 1e3) / 2000.0;
}

#ifdef __linux__
/* (sysconf's count of cores is the caller's affinity's: a task's is one) */
static int pin(pthread_t t, int core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(t, sizeof set, &set);
}

static int core_now(void)
{
    return sched_getcpu();
}
#endif

static void task_a(void *arg)
{
    (void)arg;
    thread_a = pthread_self();
    r.domain_a = ros_ld32(ROS_ZP_DOMAINID);
    marks(0xAAAA0001u, 0xA5A5A5A5u, 0x01000011u);
    struct ros_handler h;
    if (!ROS_TRY(&h)) {
        r.a_handler_intact = 0;         /* B's raise must never land here */
        return;
    }
    r.a_handler_intact = 1;
    ros_task_switch(B);

    r.a_kept = marks_kept(0xAAAA0001u, 0xA5A5A5A5u, 0x01000011u) &&
               ros_ld32(ROS_ZP_DOMAINID) == r.domain_a;

    /* A blocking wait holding the baton: background work, but not B. */
    unsigned before = r.b_pings;
    uint32_t t0 = mg();
    struct timespec ts = { 0, 80 * 1000 * 1000 };
    ROS_BLOCKING(nanosleep(&ts, NULL));
    r.ticks_while_waiting = mg() - t0;
    r.b_ran_while_a_waited = r.b_pings != before;
    ros_handler_pop(&h);

    /* Ping-pong: each round is two switches. */
    unsigned p0 = r.b_pings;
    r.switch_us = pingpong();
    r.pings_timed = r.b_pings - p0;
#ifdef __linux__
    /* Pinned: both to core 1, then A to 1 and B to 2. Where the next
     * task runs is the scheduler's choice above, and costs differ. */
    cpu_set_t was_a, was_b;
    if (!pthread_getaffinity_np(thread_a, sizeof was_a, &was_a) &&
        !pthread_getaffinity_np(thread_b, sizeof was_b, &was_b) && !pin(thread_b, 2) &&
        !pin(thread_b, 1) && !pin(thread_a, 1)) {
        r.same_core = core_now() == 1;
        r.same_us = pingpong();
        if (!pin(thread_b, 2)) {
            r.cross_us = pingpong();
            r.core_a = core_now();
        }
    }
    pthread_setaffinity_np(thread_a, sizeof was_a, &was_a);    /* the tasks' core again */
    pthread_setaffinity_np(thread_b, sizeof was_b, &was_b);
#endif
    r.a_kept &= marks_kept(0xAAAA0001u, 0xA5A5A5A5u, 0x01000011u);
    r.stop = 1;
    ros_task_switch(B);                 /* B ends; the baton goes to task 0 */
    r.a_done = 1;                       /* task 0 brought it back */
}

static void task_b(void *arg)
{
    (void)arg;
    thread_b = pthread_self();
    r.domain_b = ros_ld32(ROS_ZP_DOMAINID);
    marks(0xBBBB0002u, 0x5B5B5B5Bu, 0x01000022u);
    struct ros_handler h;
    if (ROS_TRY(&h)) {
        ros_raise(ros_error(0x12345, "B's own error"));
    } else {
        r.b_caught_own = h.error && h.error->errnum == 0x12345;
    }
    ros_task_switch(A);
    while (!r.stop) {
        r.b_pings++;
        ros_task_switch(A);
    }
    r.b_kept = marks_kept(0xBBBB0002u, 0x5B5B5B5Bu, 0x01000022u) &&
               ros_ld32(ROS_ZP_DOMAINID) == r.domain_b;
    r.b_done = 1;
}

void ros_selftest_tasks(void)
{
    memset(&r, 0, sizeof r);
    struct ros_task *zero = ros_task_current();
    /* DomainId is the Wimp's from its initialisation on: its no-task
     * handle, as in RISC OS, not the runtime's 0. */
    check(zero && ros_task_id(zero) == 0, "tasks: /init is task 0", NULL);
    ros_st32(MARK_AT, 0x00C0FFEEu);
    uint32_t fpsr0 = ros_fp_current->fpsr, depth0 = ros_call_depth, sp0 = ros_svc_sp;

    A = ros_task_create(0x10000, task_a, NULL);
    B = ros_task_create(0x10000, task_b, NULL);
    check(A && B && ros_task_id(A) != ros_task_id(B) && ros_task_id(A) && !r.domain_a,
          "tasks: two created, each with a 64 KB slot, waiting for the baton", NULL);
    if (!A || !B)
        return;

    ros_task_switch(A);                 /* ...and back, when B ends */

    check(r.domain_a == ros_task_id(A) && r.domain_b == ros_task_id(B) &&
              !pthread_equal(thread_a, thread_b) && !pthread_equal(thread_a, pthread_self()),
          "tasks: each runs on its own thread, DomainId its own", "A %u/%u, B %u/%u",
          r.domain_a, ros_task_id(A), r.domain_b, ros_task_id(B));
    check(r.a_kept && r.b_kept,
          "tasks: the baton brings back each task's slot, SVC stack and floating point",
          "A %d, B %d", r.a_kept, r.b_kept);
    check(r.b_caught_own && r.a_handler_intact,
          "tasks: a raise stays in its task -- B's error did not reach A's handler", NULL);
    check(!r.b_ran_while_a_waited && r.ticks_while_waiting >= 5,
          "tasks: while the task with the baton waits, background work runs, other tasks not",
          "B ran %d, %u ticks", r.b_ran_while_a_waited, r.ticks_while_waiting);
    check(r.pings_timed == 1000 && r.switch_us > 0 && r.switch_us < 200,
          "tasks: 2000 switches", "%u rounds, %.1f us a switch", r.pings_timed, r.switch_us);
    ros_console_printf("        %.1f us a switch: two mmaps and a wake-up, on one CPU\n",
                       r.switch_us);
    if (r.same_us > 0)
        ros_console_printf("        %.1f us a switch with both tasks on core 1 (A on %s), %.1f us with "
                           "B on core 2 (A on core %d)\n", r.same_us, r.same_core ? "it" : "another",
                           r.cross_us, r.core_a);
    check(r.b_done && ros_task_ended(B) && !ros_task_ended(A),
          "tasks: B returned, and the baton went to its creator", NULL);
    check(ros_task_current() == zero && ros_ld32(ROS_ZP_DOMAINID) == 0 &&
              ros_ld32(MARK_AT) == 0x00C0FFEEu && ros_fp_current->fpsr == fpsr0 &&
              ros_call_depth == depth0 && ros_svc_sp == sp0 && ros_slot_current == NULL,
          "tasks: task 0 is itself again -- SVC stack, floating point, depth, no slot", NULL);

    ros_task_switch(A);
    check(r.a_done && ros_task_ended(A) && ros_task_current() == zero,
          "tasks: A resumed where it switched, finished, and came back", NULL);
    ros_task_destroy(A);
    ros_task_destroy(B);
}
