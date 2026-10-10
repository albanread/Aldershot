/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* meter.c: how busy the box is. Three counters, read by OS_ReadSysInfo 100.
 *
 * Only the runtime can see this. Every SWI is timed as it dispatches
 * (ros_swi, and the native calls beside it). Every wait for something to do
 * is timed where the box waits (ros_idle, with the lock let go and nothing
 * pending). Portable_Idle sits in the same wait. From two reads of the
 * counters, the busyness of the window between them follows:
 *
 *   busy = 1 - delta(idle_ns) / delta(now_ns)      the box found nothing to do
 *   in_swi = delta(swi_ns) / delta(now_ns)         the box spent inside SWIs
 *
 * now_ns is the clock that the other two are measured with. It is
 * monotonic from the process's start.  The block OS_ReadSysInfo 100 fills is six words:
 * now_ns, swi_ns, idle_ns, each 64 bits little-endian.
 */
#include <stdint.h>
#include <time.h>

#include "rosgd/arena.h"
#include "rosgd/meter.h"

uint64_t ros_meter_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t swi_ns, idle_ns, t0_ns;

void ros_meter_init(void)
{
    t0_ns = ros_meter_now_ns();
}

void ros_meter_add_swi(uint64_t ns)
{
    swi_ns += ns;
}

void ros_meter_add_idle(uint64_t ns)
{
    idle_ns += ns;
}

/* the block: now, then the two counters, each 64 bits */
void ros_meter_read(uint32_t block)
{
    uint64_t now = ros_meter_now_ns() - t0_ns;
    ros_st32(block + 0, (uint32_t)now);
    ros_st32(block + 4, (uint32_t)(now >> 32));
    ros_st32(block + 8, (uint32_t)swi_ns);
    ros_st32(block + 12, (uint32_t)(swi_ns >> 32));
    ros_st32(block + 16, (uint32_t)idle_ns);
    ros_st32(block + 20, (uint32_t)(idle_ns >> 32));
}
