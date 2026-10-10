/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* sysvars.h -- system variables and GSTrans (runtime/sysvars.c).
 *
 * The kernel's own, reimplemented: OS_ReadVarVal, OS_SetVarVal, OS_GSInit,
 * OS_GSRead and OS_GSTrans are raw SWIs over them, and callers see what the
 * kernel's callers saw. The variables are nodes in the RMA in the kernel's
 * layout, name first, so the name pointer OS_ReadVarVal returns in R3 is
 * one a program can print and pass back to go on enumerating.
 */
#ifndef ROSGD_SYSVARS_H
#define ROSGD_SYSVARS_H

#include <stdint.h>

/* The kernel's InitVariables: Sys$Time, Sys$Date, Sys$Year,
 * Sys$ReturnCode, Sys$RCLimit, Sys$DateFormat and Alias$. They are set
 * before any module starts, as the kernel sets them before it starts the
 * ROM's. */
void ros_sysvars_init(void);

/* The kernel's VarFindIt_QA (Kernel/s/Arthur2, Oscli_QuickAliases): one
 * name found by binary chop, without a SWI and without an error. The
 * result is the variable's node, or 0. The name ends at a space or below,
 * and case does not matter. * and # in it are not wildcards here.  OS_CLI looks an alias
 * up with it (runtime/oscli.c). */
uint32_t ros_sysvars_find_quick(uint32_t name);

#endif
