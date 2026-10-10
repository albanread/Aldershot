/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* armbox.h -- the ARM container, as the runtime sees it (runtime/armrun/box.c).
 *
 * An &FF8 file of ARM code, an AIF image or an Absolute and not a C
 * application's ELF image (capp.h), runs as the task's application under
 * the engine and the bridge. FileSwitch's *Run sends it here. */
#ifndef ROSGD_ARMBOX_H
#define ROSGD_ARMBOX_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

/* *Run of ARM code: path the canonical name, line the command line as *Run
 * had it.  An error when it cannot start (26-bit code, no memory); after
 * that, its end is the runtime's, as any application's. */
os_error *ros_armrun_run(const char *path, uint32_t line);

/* ros_call's fourth kind: code in the running ARM task's memory, called on
 * a nested engine (the task is inside a SWI).  1 when it was ARM code. */
int ros_armrun_call(struct ros_cpu *s, uint32_t addr);

/* Whether this thread is running an ARM task */
int ros_armrun_active(void);

/* ARM code in the RMA that is not an application's: [lo, hi) is ARM
 * code from now, such as a module loaded from a file, until it is removed.
 * The result is 0 if no room is left to record it */
int ros_armrun_code_add(uint32_t lo, uint32_t hi);
void ros_armrun_code_remove(uint32_t lo);
/* Every range overlapping [lo, hi) forgotten: something else is there now */
void ros_armrun_code_forget(uint32_t lo, uint32_t hi);
/* Whether addr is in ARM code: a module, utility or BASIC block the engine
 * runs (*NModules's A) */
int ros_armrun_is_code(uint32_t addr);
/* Native code is at [lo, hi) now (capp.h's ranges): what the engines
 * translated there goes, so ARM code that branches there calls it (#146) */
void ros_armrun_code_native(uint32_t lo, uint32_t hi);

/* The emulated SharedCLibrary (RISC OS 5.30's) in the RMA, started if it
 * was not: where a 5.30 ROM C module's library calls go.  0 if it cannot. */
uint32_t ros_armrun_clib(void);

/* *ARMStats: what the ARM container has cost. It is given a line at a time
 * to emit (which returns non-zero to stop). With reset, the counts are
 * begun again */
void ros_armrun_stats(int reset, int (*emit)(const char *line, void *ctx), void *ctx);

/* The same, for DeskMeter's ARM window: running totals since the start
 * (or *ARMStats -reset). They are the time in the engine, the time in
 * native code that ARM code called, the time in the FPA, and the counts of
 * instructions, SWIs and calls in. They also give what is there now, and
 * where ARM code has run most (by the PC samples) */
struct ros_arm_view {
    uint64_t ns_engine, ns_native, ns_fpa, instructions, swis, calls, samples;
    unsigned tasks, call_tasks, modules;
    unsigned hot;                               /* how many of hot_* */
    uint32_t hot_count[3];
    char hot_name[3][48];
};
void ros_armrun_view(struct ros_arm_view *v);

/* *Run of a utility (&FFC): path the file, line the command line, tail
 * its tail (both guest addresses) */
os_error *ros_armrun_utility(const char *path, uint32_t line, uint32_t tail);

#endif
