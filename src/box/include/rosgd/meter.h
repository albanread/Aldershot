/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* meter.h -- how busy the box is (meter.c), read by OS_ReadSysInfo 100. */
#ifndef ROSGD_METER_H
#define ROSGD_METER_H

#include <stdint.h>

void ros_meter_init(void);
uint64_t ros_meter_now_ns(void);
void ros_meter_add_swi(uint64_t ns);    /* a SWI's whole dispatch */
void ros_meter_add_idle(uint64_t ns);   /* a wait with nothing to do */
/* six words at block: now_ns, swi_ns, idle_ns, 64 bits each */
void ros_meter_read(uint32_t block);

#endif
