/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* switrace.h -- what the SWIs did: a ring of the latest calls, and counts.
 *
 * Every SWI, through the dispatcher (ros_swi) or a compiled module's
 * direct call of a native one (ros_native_swi), is recorded as it goes in
 * and completed as it comes out. The record holds when (ns since start),
 * how long, the SWI, R0-R3 in, R0 out or the error, the depth, and the
 * task (DomainId). The ring keeps the latest ROSGD_SWIRING calls (default
 * 131072; 0 turns recording off). The counts, per SWI, are calls, errors,
 * total time, self time (less the SWIs it called) and the longest, and
 * they run from start or *SWIStats -reset. A call the same as the one
 * before it (SWI, task, depth, R0-R3 in) folds into that record, with a
 * count, the last time and the latest result.
 *
 *   *SWITrace [<n>]          the last n calls (default 40), oldest first
 *   *SWIStats [-all|-reset]  per SWI, by total time: the top 40, or all
 *   ROSGD_SWITRACE=1|all     each call printed to stderr as well, live ("1"
 *                            leaves out the character output SWIs)
 *   rosgd.switrace[=all]     the same in the box, on the kernel command line
 *                            (stderr is the serial console there);
 *                            rosgd.swiring=<n> is ROSGD_SWIRING
 *   ROSGD_SWISTATS=<file>    the counts and the whole ring written at exit
 *
 * A return to the wrong place ("Return to &X where &Y was expected") prints
 * the last 64 calls to stderr, which show what led up to it.
 */
#ifndef ROSGD_SWITRACE_H
#define ROSGD_SWITRACE_H

#include <stdint.h>
#include <stdio.h>

struct ros_cpu;

/* The kind of code that made a SWI: ARM, an SVC the ARM engine executed
 * (the bridge's box_swi), or native, which is any other, whatever task the
 * thread runs. ARM callers reach a module's ARM shadow, and native callers
 * the module in the chain. */
#define ROS_KIND_NATIVE 0
#define ROS_KIND_ARM    1

/* One SWI in flight: the caller's, on its own stack (task threads each
 * have theirs), linking to the call it is inside.  Linked whether or not
 * SWIs are traced: its kind is the caller kind of what the SWI does. */
struct ros_swi_frame {
    uint64_t t0, child_ns;
    uint64_t seq;                   /* its record in the ring, or 0 */
    uint32_t number;
    uint8_t kind;                   /* ROS_KIND_*: who made the call */
    struct ros_swi_frame *outer;
};

void ros_switrace_init(void);
/* The kind of the innermost SWI on this thread (ROS_KIND_*): the caller on
 * whose behalf the runtime is acting.  Native with no SWI in flight (the
 * background: tickers, callbacks, interrupts). */
int ros_caller_kind(void);
/* Whether SWIs are traced live (ROSGD_SWITRACE): for other events worth a line */
int ros_switrace_live(void);
void ros_switrace_in(struct ros_swi_frame *f, uint32_t number, const struct ros_cpu *s);
void ros_switrace_out(struct ros_swi_frame *f, const struct ros_cpu *s);
/* The innermost call traced, and, after a longjmp out of SWIs back to
 * where it was read, that call the innermost again (module.c). */
struct ros_swi_frame *ros_switrace_top(void);
void ros_switrace_unwind(struct ros_swi_frame *top);
/* A longjmp to the frame `to` (dispatch.c's ros_resume_unwind): the calls
 * traced in frames below it have ended, so none is left as an outer call
 * whose dead frame a later exit would write. */
void ros_switrace_unwind_below(const void *to);

/* The last n calls, oldest first, as text lines to emit (which stops the
 * listing by returning non-zero). */
int ros_switrace_recent(unsigned n, int (*emit)(const char *line, void *ctx), void *ctx);
/* The counts by total time: the top `top` (0 all). */
int ros_switrace_stats(unsigned top, int (*emit)(const char *line, void *ctx), void *ctx);
void ros_switrace_reset(void);
void ros_switrace_dump(FILE *f, unsigned n);

#endif
