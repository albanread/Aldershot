/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* ticker.h -- the centisecond tick: MetroGnome, TickerV, OS_CallAfter and
 * OS_CallEvery.
 *
 * On RISC OS a hardware timer interrupts 100 times a second. The kernel's
 * handler (Kernel/s/NewIRQs, TickOne) counts MetroGnome up, calls TickerV,
 * then runs the ticker events that OS_CallAfter and OS_CallEvery set
 * (Kernel/s/TickEvents). Under ROSGD the interrupt is a pump thread
 * (runtime/ticker.c) that wakes every centisecond and posts the tick to the
 * event queue. The tick's work is background work, run holding the lock at
 * the next safe point (background.h).
 *
 * If safe points are far apart, because module code runs long, ticks wait.
 * They are then run one by one until MetroGnome has caught up with the
 * clock. They are late but none is lost, and each ticker event fires on its
 * own count.
 */
#ifndef ROSGD_TICKER_H
#define ROSGD_TICKER_H

#include <stdint.h>

#include "rosgd/error.h"

/* Start the pump: MetroGnome counts from now.  After ros_background_start. */
int ros_ticker_start(void);

/* Centiseconds since the ticker started: what OS_ReadMonotonicTime gives,
 * read from the clock, so current even while ticks wait for a safe point.
 * MetroGnome in zero page is the count of ticks run so far. */
uint32_t ros_monotonic_cs(void);

/* The kernel's ticker events. After cs + 1 ticks, which is RISC OS's own
 * compensation for callers who subtract one, code is called with R12.
 * With every non-zero, it is called again every + 1 ticks after that. An
 * interval of 0, or of &FFFFFC00 or more, is BadTime. */
os_error *ros_ticker_add(uint32_t cs, uint32_t every, uint32_t code, uint32_t r12);
os_error *ros_ticker_remove(uint32_t code, uint32_t r12);

/* The timers OS_Word keeps (the system clock and the interval timer) and
 * the 50 Hz VSync, moved on each tick (runtime/osbyte.c). */
/* last: the final tick of a batch the ticker runs to catch up with the
 * clock. The centisecond counts all catch up. A VSync falls due every
 * second tick but is delivered only on the last one of a batch, at most
 * one however late the ticker ran, as a VSync interrupt missed while the
 * machine was busy is lost and not queued. */
void ros_timers_tick(int last);
uint32_t ros_vsync_count(void);

#endif
