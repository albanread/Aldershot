/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2011 Castle Technology Ltd
 * Copyright 2015 Castle Technology Ltd
 * Copyright 2016 Castle Technology Ltd
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
 * This file is a reimplementation in C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.Middle, s.Kernel, s.CPUFeatures, hdr.OSRSI6,
 * hdr.OSMisc).
 */
/* sysinfo.c -- OS_ReadSysInfo, as the kernel has it (Kernel/s/Middle,
 * ReadSysInfo_Code), with the answers of a machine that is not a HAL
 * platform. Most reasons report hardware, and ROSGD has none of it.
 *
 *   0  the configured screen size: the display's framebuffer
 *   1  the configured mode, monitor type and sync: the mode the display
 *      starts in, with multisync and separate syncs. These are ROSGD's
 *      choice, because there is no CMOS and the kernel's own answer
 *      depends on the HAL
 *   2  the chips: none. There is no IOEB, IOC/IOMD, MEMC or VIDC, and no
 *      ID
 *   3  the I/O chip's features: none
 *   4  an Ethernet address from the ID chip: none (the network is Linux's)
 *   5  the ID chip's raw data: none
 *   6  kernel workspace values: each is "no longer meaningful", so 0, as
 *      the call's contract allows
 *   7  the last unexpected abort: none recorded
 *   8  the platform: 0, unspecified, whose flags are 0
 *   9  the ROM's strings: the OS name (also 6), the build date, and the
 *      platform (hardware.c's name: "QEMU virt" on the HAL, else "Linux")
 *  11  debug character I/O: unknown, as without the HAL's DebugTX/RX
 *  12  the extended machine ID: none
 *  13  key handler flags: the kernel's (wide key numbers)
 *  14  IIC buses: none
 *  15  the extended ROM footer: empty
 *
 * Reasons 10 (RISCOS Ltd's "read OS version") and 16 upwards are unknown,
 * as they are there. The exception is reason 100, which is the box's own.
 * R1 points at a block of six words. They are filled with now_ns, swi_ns
 * and idle_ns (meter.c), each 64 bits little-endian. These are the
 * runtime's busyness counters behind DeskMeter's graph.
 *
 * OS_PlatformFeatures (&6D) follows the kernel's PlatFeatSWI
 * (Kernel/s/Kernel, s/CPUFeatures), for the box's machine. Native
 * programs are x86-64 or AArch64 C, and BASIC's assembler is the host's.
 * So whatever describes ARM instructions is absent, and is known to be.
 * ARM programs, which run in the emulated container, are given the same
 * answers. The reasons are:
 *
 *   0  code features: R0 is the kernel's ProcessorFlags. It describes a
 *      32-bit OS with no 26-bit mode, no SWP (and no LDREX:
 *      CPUFlag_LoadStoreEx is clear, so the C library's _swp_available is
 *      0), and no physical pages. Unknown reasons are refused properly.
 *      Caches are split if the host's are. Code areas are to be
 *      synchronised on AArch64, where OS_SynchroniseCodeAreas cleans the
 *      instruction cache. R1 is 0, meaning no interrupt-delay routine
 *  32  the processor vectors: none. R0 is 0 and R1 is 0, with no room for
 *      a handler
 *  33  cache level R1 (0-based): the host CPU's, as Linux's sysfs (or, run
 *      hosted on macOS, sysctl) describes it. R0 is 1 for instruction, 2
 *      for data, 3 for split, 4 for unified and 0 for none. R1-R4 are the
 *      D line and size and the I line and size. They are 0 where the host
 *      does not say. VZ's sysfs gives no sizes, so the lines are then
 *      CTR_EL0's
 *  34  the CPUFeature_ flags: each ARM instruction group is known absent.
 *      For one flag the answer is 0, or -1 past CPUFeature_Max. For page 0
 *      (R1 -1), R0-R3 are 0 and R4-R7 are the validity masks. Any other
 *      page is all 0
 *  35  the routine that clears the exclusive monitor: none, R1 0
 *
 * OS_Reset (&6A) restarts the machine, or powers it off, and does not
 * return. ReadSysInfo 8's flags say there is no software power-off, as a
 * Pi's do. The farm's 5.30 Switcher offers its "ready to be switched off"
 * dialogue with Restart, so the desktop's Shutdown ends at Restart.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/meter.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"

#define SERVICE_PRERESET 0x45u
#define POWER_OFF_MAGIC 0x46464F26u     /* '&OFF' */
#define ERR_BAD_READSYSINFO 0x1ECu
#define ERR_BAD_KEYHANDLER 0x1F4u

#define KEYHANDLER_KEYTRANSIZE 4
#define KEYHANDLER_FLAGS 32
#define KEYHANDLER_HASFLAGS 0x80000000u
#define KEYHANDLER_FLAG_WIDE 0x00000001u

/* The strings reason 9 points at, in the arena */
static uint32_t os_name, build_date;

static uint32_t arena_string(const char *text)
{
    size_t n = strlen(text) + 1;
    char *p = ros_rma_alloc((uint32_t)n);
    if (!p)
        return 0;                       /* the RMA is full */
    memcpy(p, text, n);
    return ros_addr(p);
}

/* The build date, in OS_Word 14's format ("%w3,%dy %m3 %ce%yr.%24:%mi:%se").
 * It is ROSGD 0.01's date and is fixed. The box's builds are reproducible,
 * so __DATE__ is not used. */
static uint32_t make_build_date(void)
{
    return arena_string("Fri,25 Sep 2026.00:00:00");
}

/* OS_ReadSysInfo 6: where the kernel keeps an item (Kernel/hdr/OSRSI6).
 * This is answered for the items that ROSGD's zero page has at their
 * KernelWS offsets. The answer is 0 for the rest. Callers take 0 as "not
 * known here". Otherwise the Wimp would fall back to the legacy low
 * addresses, which are not mapped. */
static uint32_t kernel_item(uint32_t item)
{
    switch (item) {
    case 69:                                    /* OSRSI6_IRQsema */
        return ROS_ZP_IRQSEMA;
    case 70:                                    /* OSRSI6_DomainId */
        return ROS_ZP_DOMAINID;
    case 77:                                    /* OSRSI6_MetroGnome */
        return ROS_ZP_METROGNOME;
    case 78:                                    /* OSRSI6_CLibCounter */
        return ROS_ZP_CLIBCOUNTER;
    case 79:                                    /* OSRSI6_RISCOSLibWord */
        return ROS_ZP_RISCOSLIBWORD;
    case 80:                                    /* OSRSI6_CLibWord */
        return ROS_ZP_CLIBWORD;
    default:
        return 0;
    }
}

/* OS_Reset (Kernel s/PMF/key, PerformReset). Service_PreReset is offered
 * to every module. Then the machine restarts. With R0 '&OFF' it is turned
 * off instead (HAL_Reset), as the Switcher's Shutdown asks when the
 * platform can. The box then powers off, as at the end of make boot. It
 * does not return, except to a self-test's hook. */
void (*ros_reset_hook)(uint32_t r0);

void ros_thunk_OS_Reset(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0];
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = SERVICE_PRERESET;
    ros_service_call(&c);
    s->v = 0;
    if (ros_reset_hook) {
        ros_reset_hook(r0);
        return;
    }
    if (r0 == POWER_OFF_MAGIC)
        ros_poweroff(0);
    ros_restart();
}

void ros_thunk_OS_ReadSysInfo(struct ros_cpu *s)
{
    s->v = 0;
    switch (s->r[0]) {
    case 0:
        s->r[0] = ros_vdu_screen_size();
        return;
    case 1: {
        s->r[0] = ros_vdu_start_mode();
        s->r[1] = 1;                    /* multisync */
        s->r[2] = 0;                    /* separate syncs */
        return;
    }
    case 2:
        s->r[0] = 0xFFFFFF00u;          /* no IOEB, and IOC, MEMC and VIDC absent */
        s->r[1] = 0, s->r[2] = 0, s->r[3] = 0, s->r[4] = 0;
        return;
    case 3:
        s->r[0] = 0, s->r[1] = 0;
        return;
    case 4:
    case 5:
        s->r[0] = 0, s->r[1] = 0;
        return;
    case 6:
        if (s->r[1] == 0) {
            s->r[2] = kernel_item(s->r[2]);
        } else {
            uint32_t in = s->r[1], out = s->r[2];
            for (;; in += 4, out += 4) {
                uint32_t item = ros_ld32(in);
                if (item == 0xFFFFFFFFu)
                    break;
                ros_st32(out, kernel_item(item));
            }
        }
        return;
    case 7:
        s->r[1] = 0, s->r[2] = 0, s->r[3] = 0;
        return;
    case 8:
        s->r[0] = 0, s->r[1] = 0, s->r[2] = 0;
        return;
    case 9:
        switch (s->r[1]) {
        case 0:
        case 6:
            if (!os_name)
                os_name = arena_string("ROSGD 0.01");
            s->r[0] = os_name;
            return;
        case 2:
            if (!build_date)
                build_date = make_build_date();
            s->r[0] = build_date;
            return;
        case 7:
            s->r[0] = ros_platform_name_addr();     /* hardware.c */
            return;
        default:
            s->r[0] = 0;
            return;
        }
    case 12:
        s->r[0] = 0;
        return;
    case 13: {
        uint32_t h = s->r[1];
        s->r[0] = KEYHANDLER_FLAG_WIDE;
        if (!h)
            return;
        uint32_t size = ros_ld32(h + KEYHANDLER_KEYTRANSIZE);
        uint32_t flags = size & KEYHANDLER_HASFLAGS ? ros_ld32(h + KEYHANDLER_FLAGS) : 0;
        if (flags & ~KEYHANDLER_FLAG_WIDE)
            ros_swi_fail(s, ros_error(ERR_BAD_KEYHANDLER, "Bad key handler"));
        return;
    }
    case 14:
        s->r[0] = 0;
        return;
    case 15:
        s->r[0] = 15, s->r[1] = 0;
        return;
    case 100:                                   /* the box's own: meter.c */
        if (s->r[1] >= 0x8000u) {
            ros_meter_read(s->r[1]);            /* now, swi, idle ns each */
            return;
        }
        ros_swi_fail(s, ros_error(ERR_BAD_READSYSINFO, "A block to fill, in R1"));
        return;
    default:                            /* 10, 11 and 16 up */
        ros_swi_fail(s, ros_error(ERR_BAD_READSYSINFO, "Unknown OS_ReadSysInfo call"));
        return;
    }
}

/* ---- OS_PlatformFeatures ------------------------------------------------------ */

#define ERR_BAD_PLATREAS 0x1F0u

#define CPUFLAG_SYNCHRONISECODEAREAS (1u << 0)
#define CPUFLAG_SPLITCACHE           (1u << 5)
#define CPUFLAG_32BITOS              (1u << 6)
#define CPUFLAG_NO26BITMODE          (1u << 7)
#define CPUFLAG_NOSWP                (1u << 11)
#define CPUFLAG_NOPHYSICALPAGES      (1u << 22)
#define CPUFLAG_EXTRAREASONCODESFIXED (1u << 31)

#define CPUFEATURE_MAX 65               /* Kernel/hdr/OSMisc, 5.30's */

/* One level of the host's caches, as reason 33 gives it */
struct cache_level {
    uint32_t type, dline, dsize, iline, isize;
};

#ifdef __APPLE__
static uint32_t sysctl_u32(const char *name)
{
    uint64_t v = 0;                     /* 4 or 8 bytes. Either works, as it is little-endian. */
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, NULL, 0) != 0)
        return 0;
    return (uint32_t)v;
}

static void cache_info(uint32_t level, struct cache_level *c)
{
    memset(c, 0, sizeof *c);
    uint32_t line = sysctl_u32("hw.cachelinesize");
    if (level == 0) {
        c->dsize = sysctl_u32("hw.l1dcachesize");
        c->isize = sysctl_u32("hw.l1icachesize");
        c->dline = c->dsize ? line : 0;
        c->iline = c->isize ? line : 0;
        c->type = (c->isize ? 1 : 0) | (c->dsize ? 2 : 0);
    } else if (level == 1 && (c->dsize = sysctl_u32("hw.l2cachesize")) != 0) {
        c->type = 4;
        c->isize = c->dsize, c->dline = c->iline = line;
    }
}
#else
/* Linux: /sys/devices/system/cpu/cpu0/cache/indexN. Each has its level
 * (1-based), type (Data, Instruction, Unified), line size and size
 * ("32K"). */
static int read_line(const char *dir, const char *leaf, char *buf, size_t n)
{
    char path[96];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    return ok;
}

static void cache_info(uint32_t level, struct cache_level *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 16; i++) {
        char dir[64], buf[32];
        snprintf(dir, sizeof dir, "/sys/devices/system/cpu/cpu0/cache/index%d", i);
        if (!read_line(dir, "level", buf, sizeof buf))
            break;
        if ((uint32_t)strtoul(buf, NULL, 10) != level + 1)
            continue;
        uint32_t line = 0, size = 0;
        if (read_line(dir, "coherency_line_size", buf, sizeof buf))
            line = (uint32_t)strtoul(buf, NULL, 10);
        if (read_line(dir, "size", buf, sizeof buf)) {
            char *end;
            size = (uint32_t)strtoul(buf, &end, 10);
            if (*end == 'K')
                size <<= 10;
            else if (*end == 'M')
                size <<= 20;
        }
        if (!read_line(dir, "type", buf, sizeof buf))
            continue;
        if (buf[0] == 'D' || buf[0] == 'U')
            c->dline = line, c->dsize = size;
        if (buf[0] == 'I' || buf[0] == 'U')
            c->iline = line, c->isize = size;
        c->type |= buf[0] == 'I' ? 1 : buf[0] == 'D' ? 2 : 4;
    }
    if (c->type & 4)
        c->type = 4;                    /* unified, whatever else */
#if defined(__aarch64__)
    /* Under VZ, sysfs has the levels and types but no sizes. The smallest
     * line lengths are then CTR_EL0's (DminLine and IminLine, each the log2
     * of a number of words). */
    if (c->type && !c->dline && !c->iline) {
        uint64_t ctr;
        __asm__("mrs %0, ctr_el0" : "=r"(ctr));
        if (c->type & 6)
            c->dline = 4u << ((ctr >> 16) & 15);
        if (c->type & 5)
            c->iline = 4u << (ctr & 15);
    }
#endif
}
#endif

static uint32_t processor_flags(void)
{
    struct cache_level l1;
    cache_info(0, &l1);
    uint32_t f = CPUFLAG_32BITOS | CPUFLAG_NO26BITMODE | CPUFLAG_NOSWP |
                 CPUFLAG_NOPHYSICALPAGES | CPUFLAG_EXTRAREASONCODESFIXED;
#if defined(__aarch64__)
    f |= CPUFLAG_SYNCHRONISECODEAREAS;
#endif
    if (l1.type == 3)
        f |= CPUFLAG_SPLITCACHE;
    return f;
}

void ros_thunk_OS_PlatformFeatures(struct ros_cpu *s)
{
    s->v = 0;
    switch (s->r[0]) {
    case 0:                                     /* ReadCodeFeatures */
        s->r[0] = processor_flags();
        s->r[1] = 0;
        return;
    case 32:                                    /* ReadProcessorVectors */
        s->r[0] = 0, s->r[1] = 0;
        return;
    case 33: {                                  /* ReadCacheInfo */
        struct cache_level c;
        cache_info(s->r[1], &c);
        s->r[0] = c.type, s->r[1] = c.dline, s->r[2] = c.dsize;
        s->r[3] = c.iline, s->r[4] = c.isize;
        return;
    }
    case 34: {                                  /* ReadCPUFeatures */
        int32_t which = (int32_t)s->r[1];
        if (which >= 0) {
            s->r[0] = which < CPUFEATURE_MAX ? 0 : 0xFFFFFFFFu;
        } else {
            for (int i = 0; i < 8; i++)
                s->r[i] = 0;
            if (which == -1) {                  /* page 0: all known, all clear */
                s->r[4] = s->r[5] = 0xFFFFFFFFu;
                s->r[6] = 0xFFFFFFFFu >> (96 - CPUFEATURE_MAX);
            }
        }
        return;
    }
    case 35:                                    /* ReadClearExclusive */
        s->r[1] = 0;
        return;
    default:
        ros_swi_fail(s, ros_error(ERR_BAD_PLATREAS, "Unknown OS_PlatformFeatures reason code"));
        return;
    }
}
