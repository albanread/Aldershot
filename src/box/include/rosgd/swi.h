/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* swi.h -- the runtime's side of SWI dispatch.
 *
 * ros_swi() (cpu.h) routes a SWI number to its implementation: a native
 * typed function, through its generated thunk; or a compiled module's SWI
 * entry, with the register block passed straight through with no conversion,
 * so compiled-to-compiled calls keep exact semantics. Where the
 * implementation lives is invisible to the caller.
 */
#ifndef ROSGD_SWI_H
#define ROSGD_SWI_H

#include "rosgd/cpu.h"
#include "rosgd/error.h"

/* OS_CallASWI and OS_CallASWIR12: ros_swi() dispatches the SWI numbered in
 * R10 or R12 in their place. */
#define OS_CALLASWI     0x6Fu
#define OS_CALLASWIR12  0x71u

/* ros_swi() for a caller of a known kind (switrace.h's ROS_KIND_*): the
 * ARM container's bridge makes ARM code's SVCs with ROS_KIND_ARM, so its
 * calls reach ARM shadows */
void ros_swi_as(struct ros_cpu *s, uint32_t number, int kind);

/* Fail a SWI: V set, R0 -> the error block (arena memory). */
void ros_swi_fail(struct ros_cpu *s, const os_error *e);

/* Index the generated native SWI table.  Before any ros_swi(). */
void ros_swi_init(void);

/* A SWI's name, for diagnostics: "OS_Write0", "T0Demo_Sum", or NULL. */
const char *ros_swi_name(uint32_t number);

#endif
