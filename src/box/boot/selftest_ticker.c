/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_ticker.c: the centisecond tick, against ticker.h and the
 * kernel it stands in for (Kernel/s/NewIRQs, Kernel/s/TickEvents).
 *
 * Ticker events are set through the SWIs, with code addresses for C
 * routines (ros_native_entry), as a module sets them with its own code.
 * Waits release the lock (ROS_BLOCKING), so ticks run on the background
 * thread while the test sleeps. One wait deliberately keeps the lock, to
 * show that ticks are deferred and then caught up with none lost.
 */
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/ticker.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void wait_ms(unsigned ms)
{
    ROS_BLOCKING(sleep_ms(ms));
}

static uint32_t mg(void)
{
    return ros_ld32(ROS_ZP_METROGNOME);
}

static uint32_t now_cs(void)
{
    uint32_t cs = 0;
    xos_read_monotonic_time(&cs);
    return cs;
}

/* A ticker routine: counts its calls, and what it saw. */
struct seen {
    unsigned calls;
    uint32_t r12, irqsema, irq_off;
    uint32_t at[8];
};
static struct seen a, b;

static void record(struct ros_cpu *s, struct seen *t)
{
    if (t->calls < 8)
        t->at[t->calls] = mg();
    t->calls++;
    t->r12 = s->r[12];
    t->irqsema = ros_ld32(ROS_ZP_IRQSEMA);
    t->irq_off = ros_irq_off_count();
    s->r[15] = s->r[14];
}

static void routine_a(struct ros_cpu *s) { record(s, &a); }
static void routine_b(struct ros_cpu *s) { record(s, &b); }

static unsigned tickerv_calls;
static int tickerv(struct ros_cpu *s, uint32_t r12)
{
    (void)s, (void)r12;
    tickerv_calls++;
    return ROS_VECTOR_PASS;
}

void ros_selftest_ticker(void)
{
    uint32_t code_a = ros_native_entry(routine_a, "selftest:ticker_a");
    uint32_t code_b = ros_native_entry(routine_b, "selftest:ticker_b");

    /* ---- MetroGnome and TickerV, a hundred times a second ---- */
    ros_vector_claim_native(ROS_TICKERV, tickerv, 0);
    wait_ms(20);
    uint32_t mg0 = mg(), t0 = now_cs();
    tickerv_calls = 0;
    wait_ms(200);
    uint32_t mg1 = mg(), t1 = now_cs(), calls = tickerv_calls;
    ros_vector_release_native(ROS_TICKERV, tickerv, 0);
    check(t1 - t0 >= 19 && t1 - t0 <= 25, "ticker: OS_ReadMonotonicTime -- 20 cs in 200 ms",
          "%u cs", t1 - t0);
    check(mg1 - mg0 >= 17 && mg1 - mg0 <= 25 && (int32_t)(t1 - mg1) <= 2,
          "ticker: MetroGnome counts the ticks, and keeps up with the clock",
          "MetroGnome +%u, clock %u, MetroGnome %u", mg1 - mg0, t1, mg1);
    check(calls == mg1 - mg0, "ticker: TickerV is called once a tick", "%u calls, %u ticks",
          calls, mg1 - mg0);

    /* ---- CallAfter: once, after cs + 1 ticks, R12 given, interrupts off ---- */
    os_error *e = xos_call_after(5, code_a, 0x1234);
    uint32_t set = mg();
    wait_ms(150);
    check(!e && a.calls == 1 && a.r12 == 0x1234 && a.at[0] - set >= 5 && a.at[0] - set <= 8,
          "ticker: OS_CallAfter 5 -- called once, six ticks on, with R12", "%s %u calls, +%u",
          e ? e->errmess : "", a.calls, a.at[0] - set);
    check(a.irqsema != 0 && a.irq_off > 0,
          "ticker: a ticker routine runs in the background, interrupts off", "IRQsema &%X, off %u",
          a.irqsema, a.irq_off);

    /* ---- CallEvery: every cs + 1, until removed ---- */
    b.calls = 0;
    e = xos_call_every(1, code_b, 7);
    wait_ms(205);
    unsigned every = b.calls;
    int spaced = 1;
    for (unsigned i = 1; i < every && i < 8; i++)
        spaced &= b.at[i] - b.at[i - 1] == 2;
    check(!e && every >= 8 && every <= 12 && spaced,
          "ticker: OS_CallEvery 1 -- every two ticks", "%u calls in 20 cs", every);
    xos_remove_ticker_event(code_b, 7);
    unsigned after = b.calls;
    wait_ms(60);
    check(b.calls == after, "ticker: OS_RemoveTickerEvent stops it", "%u more calls",
          b.calls - after);

    /* ---- the chain: order, and removal passing its wait on ---- */
    a.calls = b.calls = 0;
    xos_call_after(6, code_a, 1);
    xos_call_after(2, code_b, 2);
    xos_call_after(4, code_b, 99);
    xos_remove_ticker_event(code_b, 99);            /* from the middle of the chain */
    set = mg();
    wait_ms(150);
    check(a.calls == 1 && b.calls == 1 && b.at[0] < a.at[0] && a.at[0] - set >= 7 &&
              a.at[0] - set <= 10,
          "ticker: events fire in time order, and a removal keeps the rest on time",
          "a %u at +%u, b %u at +%u", a.calls, a.at[0] - set, b.calls, b.at[0] - set);

    e = xos_call_after(0, code_a, 0);
    check(e && e->errnum == 0x1E5, "ticker: an interval of 0 is BadTime, &1E5", "&%X",
          e ? e->errnum : 0);
    e = xos_call_every(0xFFFFFC00u, code_a, 0);
    check(e && e->errnum == 0x1E5, "ticker: so is &FFFFFC00", NULL);

    /* ---- ticks wait for a safe point, then catch up, none lost ---- */
    a.calls = 0;
    xos_call_every(0 + 1, code_a, 3);               /* every 2 ticks */
    wait_ms(15);
    uint32_t held0 = mg(), c0 = a.calls;
    sleep_ms(100);                                  /* the lock kept: module code "running" */
    uint32_t held1 = mg(), c1 = a.calls;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, XOS_ReadMonotonicTime);             /* a safe point */
    uint32_t caught = mg(), c2 = a.calls, clock = now_cs();
    xos_remove_ticker_event(code_a, 3);
    check(held1 == held0 && c1 == c0,
          "ticker: while module code runs, ticks wait", "MetroGnome +%u, %u calls",
          held1 - held0, c1 - c0);
    check(caught - held0 >= 9 && (int32_t)(clock - caught) <= 2 && c2 - c0 >= 4 &&
              c2 - c0 <= (caught - held0) / 2 + 1,
          "ticker: at the next safe point they all run -- late, none lost",
          "+%u ticks, %u calls", caught - held0, c2 - c0);
}
