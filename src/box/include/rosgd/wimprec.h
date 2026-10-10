/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* wimprec.h -- the Wimp recorder (runtime/wimprec.c): each task's events
 * and messages, written down at the SWI boundary, for the
 * differential harness (tests/wimprec). */
#ifndef ROSGD_WIMPREC_H
#define ROSGD_WIMPREC_H
#include <stdint.h>
#include "rosgd/cpu.h"

extern int ros_wimprec_on;              /* rosgd.wimprec / ROSGD_WIMPREC */

void ros_wimprec_init(void);

/* A task's own Wimp SWI (the outermost), n without its X bit, in its
 * R0-R3 as called: before the Wimp has it, and after it returns */
void ros_wimprec_call(uint32_t n, const uint32_t in[4]);
void ros_wimprec_return(uint32_t n, const uint32_t in[4], const struct ros_cpu *s);

#endif
