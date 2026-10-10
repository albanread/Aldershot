/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* cmos.h -- CMOS RAM, simulated in a file (runtime/cmos.c).
 *
 * RISC OS keeps its configuration (*Configure's settings) in 2K of
 * non-volatile memory, read and written with OS_Byte 161 and 162 and
 * OS_NVMemory. ROSGD has no such chip, and like the team's emulator it keeps
 * CMOS in a file, CMOS,ff2, in the root of the HostFS share: 2048 bytes in
 * the order OS_Byte 161 addresses them, then the OS version as a word.
 */
#ifndef ROSGD_CMOS_H
#define ROSGD_CMOS_H

#include <stdint.h>

#define ROS_CMOS_SIZE 2048u

/* The file CMOS lives in, loaded now; NULL for memory alone.  With no file
 * there yet, it is made from the defaults.  Until this is called the first
 * access chooses: $ROSGD_CMOS, or in the box /host/CMOS,ff2. */
void ros_cmos_attach(const char *path);

/* The path in use, or NULL */
const char *ros_cmos_path(void);

/* A byte, as the kernel's Read: 0 beyond the end */
uint8_t ros_cmos_read(uint32_t address);

/* A byte written, as the kernel's Write: the checksum kept, the station
 * number and the one-time-programmable bytes (&F0-&FF) left alone, the
 * file rewritten.  0, or -1 if the address is beyond the end. */
int ros_cmos_write(uint32_t address, uint8_t value);

/* The defaults: the team emulator's seed for a HostFS share, its stock CMOS
 * with FileSystem HostFS (runtime/cmos_default.bin, tools/mkcmosdefault.py),
 * 2048 bytes and the version word */
extern const uint8_t ros_cmos_default[ROS_CMOS_SIZE + 4];

/* OS_Byte 161 and 162, for osbyte.c */
struct ros_cpu;
void ros_cmos_byte(struct ros_cpu *s);

#endif
