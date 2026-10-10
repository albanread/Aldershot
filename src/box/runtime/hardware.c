/* Copyright 2000 Pace Micro Technology plc
 * Copyright 2002 Tematic Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's kernel source
 * (Sources/Kernel: s.HAL, hdr.HALEntries, hdr.HALDevice,
 * Docs.HAL.OS_Hardware).
 */

/* hardware.c implements OS_Hardware (&7A) as the kernel does (Kernel/s/HAL,
 * HardwareSWI and Docs/HAL/OS_Hardware), for a machine whose HAL is the
 * runtime itself. There is no ARM HAL underneath, so this file answers in C
 * the entries that the box can support. All other entries return the
 * kernel's "Hardware call not available" error, as a HAL's null entries do.
 *
 * Bits 0-7 of R8 give the reason code:
 *
 *   0  CallHAL. R9 is the entry number and R0-R7 are its arguments. The
 *      entry returns results in R0-R3.
 *   1  LookupRoutine. This would return the routine's address and static
 *      base, but there is no routine to call. The entries are the runtime's
 *      C functions, and a program cannot branch to them. This call
 *      therefore reports every entry as not available. A driver that looks
 *      up its entries first does without them, as it would on a HAL that
 *      provides none of them.
 *   2  DeviceAdd. R0 points to the device (Kernel/hdr/HALDevice). The call
 *      removes the device from the table if it is already there, puts it
 *      first in the table, then issues Service_Hardware 0.
 *   3  DeviceRemove. R0 points to the device. The call issues
 *      Service_Hardware 1, which a module may claim to refuse the removal;
 *      R0 then points to the module's error. Otherwise the call takes the
 *      device out of the table.
 *   4  DeviceEnumerate. Bits 0-15 of R0 give the device type, and bits
 *      16-31 the newest major version that the caller understands. R1 is
 *      where to start (0 for the first). On exit R1 is where to continue
 *      from (-1 at the end) and R2 points to the device.
 *   5  DeviceEnumerateChrono. As reason 4, but it returns the oldest device
 *      added first.
 *
 * Any other reason code gives "Bad OS_Hardware reason code".
 *
 * The box supports these entries (Kernel/hdr/HALEntries; Docs/HAL/HAL_API):
 *
 *   12-18  Timers. There is one timer, timer 0, which is the kernel's
 *          centisecond ticker. It runs at 1 MHz with a period of 10000, and
 *          callers may not reprogram it: TimerSetPeriod does nothing, because
 *          the ticker belongs to the runtime. Its countdown is the counter's.
 *   19-22  The counter. It runs at 1 MHz with a period of 10000, counting
 *          down from 9999 to 0 in each centisecond of the monotonic clock.
 *          CounterDelay sleeps.
 *   23-30  NVMemory. This is the HAL's own type (type 3), held in the CMOS
 *          file (cmos.c). Callers may read every byte, and write every byte
 *          except the station number. A write keeps the checksum correct, as
 *          OS_Byte 162 does.
 *   56-57  CPUCount returns the number of processors online. CPUNumber
 *          returns 0, the processor that RISC OS itself runs on (Worker jobs
 *          on the other processors do not call these entries).
 *   59     MachineID. There is none, so the entry returns 0.
 *   61     HardwareInfo. As OS_ReadSysInfo 2; the entry writes the result
 *          into the words that R0-R2 point to.
 *   63     PlatformInfo. As OS_ReadSysInfo 8. The entry sets no flags, since
 *          none are defined.
 *   97     PlatformName. The string is "QEMU virt" when the box runs under
 *          its HAL kernel, and "Linux" on Linux. OS_ReadSysInfo 9
 *          subreason 7 returns the same string.
 *  106     Reset. If R0 is 0 the entry turns the machine off; otherwise it
 *          restarts it. It issues no Service_PreReset, since a HAL's reset
 *          gives none.
 *  115     ExtMachineID. There is none, so the entry returns 0.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cmos.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define ERR_HW_BAD_REASON 0xC60u
#define ERR_HW_BAD_ENTRY  0xC61u
#define SERVICE_HARDWARE  0xD9u

#define COUNTER_RATE   1000000u         /* ticks per second */
#define COUNTER_PERIOD 10000u           /* one centisecond */

#define DEV_TYPE    0                   /* offset of HALDevice_Type, a halfword */
#define DEV_VERSION 8                   /* offset of HALDevice_Version */

/* ---- the platform name ---- */

/* Under the HAL, uname returns "BOX" (hal/syscall.c). Any other name is
 * Linux's or, when running hosted, the host's. The box has always called
 * these Linux. */
const char *ros_platform_name(void)
{
    static const char *name;
    if (!name) {
        struct utsname u;
        name = uname(&u) == 0 && !strcmp(u.sysname, "BOX") ? "QEMU virt" : "Linux";
    }
    return name;
}

static uint32_t name_addr;

uint32_t ros_platform_name_addr(void)
{
    if (!name_addr) {
        const char *n = ros_platform_name();
        size_t len = strlen(n) + 1;
        char *p = ros_rma_alloc((uint32_t)len);
        if (!p)
            return 0;
        memcpy(p, n, len);
        name_addr = ros_addr(p);
    }
    return name_addr;
}

/* ---- the entries ---- */

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static uint32_t counter_read(void)
{
    return COUNTER_PERIOD - 1 - (uint32_t)(now_us() % COUNTER_PERIOD);
}

static void counter_delay(uint32_t us)
{
    uint64_t until = now_us() + us;
    for (;;) {
        uint64_t now = now_us();
        if (now >= until)
            return;
        uint64_t left = until - now;
        struct timespec t = { (time_t)(left / 1000000u), (long)(left % 1000000u) * 1000 };
        nanosleep(&t, NULL);
    }
}

static void put_word(uint32_t at, uint32_t value)
{
    if (at)
        ros_st32(at, value);
}

/* Calls HAL entry n with the given registers. Returns 0 if the entry is not
 * supported. */
static int call_entry(uint32_t n, struct ros_cpu *s)
{
    uint32_t *r = s->r;
    switch (n) {
    case 12:                                    /* Timers */
        r[0] = 1;
        return 1;
    case 13:                                    /* TimerDevice */
        r[0] = 0;
        return 1;
    case 14:                                    /* TimerGranularity */
        r[0] = COUNTER_RATE;
        return 1;
    case 15:                                    /* TimerMaxPeriod */
    case 17:                                    /* TimerPeriod */
        r[0] = COUNTER_PERIOD;
        return 1;
    case 16:                                    /* TimerSetPeriod: does nothing */
        return 1;
    case 18:                                    /* TimerReadCountdown */
    case 21:                                    /* CounterRead */
        r[0] = counter_read();
        return 1;
    case 19:                                    /* CounterRate */
        r[0] = COUNTER_RATE;
        return 1;
    case 20:                                    /* CounterPeriod */
        r[0] = COUNTER_PERIOD;
        return 1;
    case 22:                                    /* CounterDelay */
        counter_delay(r[0]);
        return 1;
    /* NVMemoryType: the HAL's own type, 0-15 readable and writable */
    case 23:
        r[0] = 3 | 1u << 10 | 1u << 11;
        return 1;
    case 24:                                    /* NVMemorySize */
    case 25:                                    /* NVMemoryPageSize: one page */
        r[0] = ROS_CMOS_SIZE;
        return 1;
    case 26:                                    /* NVMemoryProtectedSize */
        r[0] = 0;
        return 1;
    case 27:                                    /* NVMemoryProtection: nothing to change */
        return 1;
    case 29: {                                  /* NVMemoryRead: R0 address, R1 buffer, R2 count */
        uint32_t k = 0;
        for (; k < r[2] && r[0] + k < ROS_CMOS_SIZE; k++)
            ros_st8(r[1] + k, ros_cmos_read(r[0] + k));
        r[0] = k;
        return 1;
    }
    case 30: {                                  /* NVMemoryWrite */
        uint32_t k = 0;
        for (; k < r[2] && r[0] + k < ROS_CMOS_SIZE; k++)
            ros_cmos_write(r[0] + k, ros_ld8(r[1] + k));
        r[0] = k;
        return 1;
    }
    case 56: {                                  /* CPUCount */
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        r[0] = c > 0 ? (uint32_t)c : 1;
        return 1;
    }
    case 57:                                    /* CPUNumber */
        r[0] = 0;
        return 1;
    case 59:                                    /* MachineID, 64 bits */
        r[0] = 0, r[1] = 0;
        return 1;
    case 61:                                    /* HardwareInfo: as OS_ReadSysInfo 2 */
        put_word(r[0], 0xFFFFFF00u);
        put_word(r[1], 0);
        put_word(r[2], 0);
        return 1;
    case 63:                                    /* PlatformInfo: as OS_ReadSysInfo 8 */
        put_word(r[1], 0);
        put_word(r[2], 0);
        return 1;
    case 97:                                    /* PlatformName */
        r[0] = ros_platform_name_addr();
        return 1;
    case 106:                                   /* Reset */
        if (r[0] == 0)
            ros_poweroff(0);
        ros_restart();
    case 115:                                   /* ExtMachineID */
        r[0] = 0;
        return 1;
    default:
        return 0;
    }
}

/* ---- the device table ---- */

static uint32_t *devices;                       /* newest device first */
static uint32_t ndevices, room;

static os_error *device_remove(uint32_t d)
{
    uint32_t i = 0;
    while (i < ndevices && devices[i] != d)
        i++;
    if (i == ndevices)
        return NULL;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 1, c.r[1] = SERVICE_HARDWARE, c.r[2] = d;
    ros_service_call(&c);
    if (c.r[1] == 0)                            /* a module claimed it to refuse, and R0 is its error */
        return ros_ptr(c.r[0]);
    /* Search again, because the service call may have changed the table. */
    for (i = 0; i < ndevices && devices[i] != d; i++)
        ;
    if (i == ndevices)
        return NULL;
    memmove(devices + i, devices + i + 1, (ndevices - i - 1) * sizeof devices[0]);
    ndevices--;
    return NULL;
}

static os_error *device_add(uint32_t d)
{
    os_error *e = device_remove(d);
    if (e)
        return e;
    if (ndevices == room) {
        uint32_t *t = realloc(devices, (room + 16) * sizeof devices[0]);
        if (!t)
            return ros_error(0x184u, "Not enough memory");
        devices = t, room += 16;
    }
    memmove(devices + 1, devices, ndevices * sizeof devices[0]);
    devices[0] = d;
    ndevices++;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = SERVICE_HARDWARE, c.r[2] = d;
    ros_service_call(&c);
    return NULL;
}

/* Finds the next device, starting at position R1, whose type matches R0 and
 * whose major version is no newer than the caller understands. If one is
 * found, R2 points to it and R1 is set to the position after it; otherwise
 * R1 is set to -1. If chrono is set, the oldest devices are searched first. */
static void device_enumerate(struct ros_cpu *s, int chrono)
{
    uint32_t type = s->r[0] & 0xFFFFu, newest = s->r[0] >> 16;
    for (uint32_t i = s->r[1]; i < ndevices; i++) {
        uint32_t d = devices[chrono ? ndevices - 1 - i : i];
        if ((ros_ld32(d + DEV_TYPE) & 0xFFFFu) == type && ros_ld32(d + DEV_VERSION) >> 16 <= newest) {
            s->r[1] = i + 1, s->r[2] = d;
            return;
        }
    }
    s->r[1] = 0xFFFFFFFFu;
}

/* ---- the SWI ---- */

void ros_thunk_OS_Hardware(struct ros_cpu *s)
{
    s->v = 0;
    switch (s->r[8] & 0xFF) {
    case 0:                                     /* CallHAL: R0-R3 returned, others preserved */
        if (!call_entry(s->r[9], s))
            break;
        return;
    case 1:                                     /* LookupRoutine: there are no routines */
        break;
    case 2: {
        os_error *e = device_add(s->r[0]);
        if (e)
            ros_swi_fail(s, e);
        return;
    }
    case 3: {
        os_error *e = device_remove(s->r[0]);
        if (e)
            ros_swi_fail(s, e);
        return;
    }
    case 4:
    case 5:
        device_enumerate(s, (s->r[8] & 0xFF) == 5);
        return;
    default:
        ros_swi_fail(s, ros_error(ERR_HW_BAD_REASON, "Bad OS_Hardware reason code"));
        return;
    }
    ros_swi_fail(s, ros_error(ERR_HW_BAD_ENTRY, "Hardware call not available"));
}
