/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* background.h -- the personality lock, the event queue, and background work.
 *
 * 32-bit RISC OS is one processor. Module code synchronises by turning
 * interrupts off, much of it is not re-entrant, and work that interrupts do
 * (tickers, events, a buffer filling) runs whenever interrupts are on,
 * even in the middle of an application's computation. ROSGD keeps those
 * guarantees with Linux threads:
 *
 *   The personality lock.  Any thread that runs module code, compiled or
 *   native, holds it, so module code never runs concurrently with module
 *   code. /init takes it at start-up and holds it, as the one task.
 *
 *   The event queue.  Linux threads (pumps waiting on sockets, timers,
 *   devices) never run module code. They post work to the queue, which
 *   any thread may do without the lock.
 *
 *   Background work.  Queued work runs holding the lock, with IRQsema
 *   non-zero, at safe points. The safe points are when the lock is
 *   released, on return from the outermost SWI, and on entry to the SWIs
 *   that poll for input, INKEY and OS_Mouse, at any depth, since a loop
 *   inside a SWI (Wimp_ReportError's) calls them waiting for what only an
 *   interrupt brings. Work also runs on the background thread, which takes
 *   the lock whenever it is free, including while a SWI waits in a
 *   blocking system call, which releases the lock for the wait. So work
 *   can run while an application computes or waits, as an interrupt
 *   would, but never in the middle of module code except where it asks for
 *   input. OS_IntOff defers it, as it deferred interrupts. The exceptions
 *   are the calls that turn interrupts on for as long as they last, as
 *   RISC OS's do: INKEY with a time limit (OS_Byte 129, R2 < &80: the
 *   kernel's RdchInkey starts with CLI; a negative INKEY does not) and
 *   OS_Byte 19's wait for VSync.
 *
 *   Transient callbacks run later still. They run on return from the
 *   outermost SWI, when no background work is running, as RISC OS runs
 *   them on the way back to user mode. They also run while INKEY or OS_ReadC
 *   waits for a key, at any depth (runtime/keyboard.c's read_wait), as the
 *   kernel's RdchLoop runs process_callbacks. That is what lets a loop
 *   inside a SWI waiting on INKEY (Wimp_ReportError's) idle, so that the
 *   Wimp's own callbacks come.
 */
#ifndef ROSGD_BACKGROUND_H
#define ROSGD_BACKGROUND_H

#include <stdint.h>

/* ---- the lock ------------------------------------------------------------ */

/* Take the lock; a thread that holds it may take it again. */
void ros_lock(void);

/* Release it once.  The outermost release first runs background work. */
void ros_unlock(void);

/* Whether this thread holds it. */
int ros_lock_held(void);

/* Around a system call that may block: release the lock entirely, so that
 * background work runs during the wait, and take it back.  errno survives. */
unsigned ros_blocking_begin(void);
void ros_blocking_end(unsigned depth);

#define ROS_BLOCKING(stmt)                                                  \
    do {                                                                    \
        unsigned ros_blocking_depth_ = ros_blocking_begin();                \
        stmt;                                                               \
        ros_blocking_end(ros_blocking_depth_);                              \
    } while (0)

/* An idle wait, as a processor waits for an interrupt (Portable_Idle's
 * WFI). Queued work is run if there is some. Otherwise the lock is
 * released until work is queued or ms milliseconds pass, and that work is
 * run. */
void ros_idle(unsigned ms);

/* ---- interrupts ---------------------------------------------------------- */

/* OS_IntOff and OS_IntOn: a nesting count, held under the lock.  While it
 * is non-zero, background work waits; the last IntOn runs it. */
void ros_irq_off(void);
void ros_irq_on(void);
unsigned ros_irq_off_count(void);

/* Interrupts on for a wait that needs them (OS_Byte 19 waits for VSync
 * with interrupts on, whatever its caller had), and back as they were. */
unsigned ros_irq_suspend(void);
void ros_irq_resume(unsigned count);

/* ---- the queue ----------------------------------------------------------- */

/* Work: a function, an argument, and a word, which is what a pump saw. */
typedef void ros_work_fn(void *arg, uint32_t info);

/* Queue work, from any thread, lock or no lock.  0, or -1 out of memory. */
int ros_post(ros_work_fn *fn, void *arg, uint32_t info);

/* A safe point, lock held: run background work now, unless interrupts are
 * off or background work is already running. */
void ros_background_run(void);

/* SWI nesting: the dispatcher counts in and out, and the way out of the
 * outermost SWI is a safe point for background work and callbacks.  A raise
 * restores the count its handler saw. */
extern uint32_t ros_call_depth;
void ros_swi_exit_outermost(void);

/* Whether this thread is running background work now. */
int ros_in_background(void);

/* ---- pumps --------------------------------------------------------------- */

/* Watch a descriptor for poll() events, once: when one comes, the watch
 * disarms itself and posts fn(arg, revents).  Watching it again re-arms it.
 * The pump is one thread for every descriptor. */
int ros_watch(int fd, short events, ros_work_fn *fn, void *arg);
void ros_unwatch(int fd);

/* A wait inside a command (or any code a task runs), as a RISC OS program
 * waits: until fd is ready for events (fd -1: none) or ms milliseconds
 * pass. It sleeps on UpCall_Sleep with a poll word that the descriptor's
 * readiness (a watch) or the time (a ticker event) sets. In a task window
 * TaskWindow claims it and polls the Wimp meanwhile, so the desktop goes
 * on. Unclaimed, with no task window, it is a blocking poll() with the
 * lock released. The result is 1 if fd is ready, 0 if not (the time
 * passed, or an early wake), and -1 with *err set if the sleep ended in an
 * error: Escape in a task window, or its death (runtime/sleep.c). */
struct os_error;
int ros_sleep_fd(int fd, short events, unsigned ms, struct os_error **err);
/* Start the background thread and the pump.  Before any module code runs
 * on another thread; /init calls it holding the lock. */
int ros_background_start(void);

#endif
