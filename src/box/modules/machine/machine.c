/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* machine.c -- Machine, a native module: what this box is running on, what
 * the kernel under it has said, and the two things a machine with no
 * keyboard of its own needs.
 *
 *   *MachineInfo            the kernel, the screen, the sound, the network,
 *                           the discs: what a boot console would have said
 *   *KernelLog [<text>]     Linux's own messages, which nothing else in the
 *                           box can reach. It is the only place a card that
 *                           would not start says so
 *   *Reboot                 restart the machine
 *   *PowerOff               stop it
 *
 * All four are for a box managed from another machine over SSH
 * (modules/sshd). A new system written to the EFI partition is of no use
 * until something restarts the machine. A fault in the hardware is
 * invisible until something prints the kernel's log.
 */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#ifdef __linux__
#include <sys/sysinfo.h>                /* the box's own kernel; a hosted build has none */
#define BOOT_CLOCK CLOCK_BOOTTIME
#else
#define BOOT_CLOCK CLOCK_MONOTONIC
#endif
#include <time.h>
#include <unistd.h>

#include "machine.h"
#include "rosgd/api.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define ERR_MACHINE 0xC8u
#define GRAPHICSV   0x2Au

static os_error *print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static os_error *print(const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    for (const char *s = text; *s; s++) {
        os_error *e = *s == '\n' ? xos_new_line() : xos_write_c((uint8_t)*s);
        if (e)
            return e;
    }
    return NULL;
}

/* GraphicsV 18 (ReadInfo) 3: what the driver calls the display */
static void screen_name(char *out, size_t max)
{
    uint8_t *buffer = ros_rma_alloc(64);        /* a SWI fills the box's memory, not ours */
    if (!buffer) {
        snprintf(out, max, "a display");
        return;
    }
    memset(buffer, 0, 64);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 3, s.r[1] = ros_addr(buffer), s.r[2] = 64, s.r[4] = 18, s.r[9] = GRAPHICSV;
    ros_swi(&s, XOS_CallAVector);
    buffer[63] = 0;
    snprintf(out, max, "%s", buffer[0] ? (const char *)buffer : "a display");
    ros_rma_free(buffer);
}

/* OS_ScreenMode 1: the mode now, as its selector gives it */
static void screen_mode(char *out, size_t max)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 1;
    ros_swi(&s, XOS_ScreenMode);
    if (s.v || s.r[1] < 256) {
        snprintf(out, max, "mode %u", s.v ? 0 : s.r[1]);
        return;
    }
    uint32_t m = s.r[1];
    uint32_t w = ros_ld32(m + 4), h = ros_ld32(m + 8), log2bpp = ros_ld32(m + 12);
    int32_t rate = (int32_t)ros_ld32(m + 16);
    char hz[24] = "an unknown rate";
    if (rate > 0)
        snprintf(hz, sizeof hz, "%d Hz", rate);
    snprintf(out, max, "%u x %u, %u bpp, %s", w, h, 1u << log2bpp, hz);
}

static os_error *cmd_info(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    os_error *e = NULL;

    struct utsname u;
    if (!uname(&u))
        e = print("Kernel      %s %s, %s\n", u.sysname, u.release, u.machine);

    struct timespec up;
    if (!e && !clock_gettime(BOOT_CLOCK, &up)) {
        unsigned long s = (unsigned long)up.tv_sec;
        e = print("Running     %lu days %lu:%02lu:%02lu\n", s / 86400, s % 86400 / 3600,
                  s % 3600 / 60, s % 60);
    }
    /* The processors the machine has. They say whether its cores are
     * reaching the box at all. The Worker module runs its jobs on the cores
     * the tasks are not using. This reads Linux's own list, not sysconf:
     * musl answers _SC_NPROCESSORS_ONLN from the caller's affinity, and a
     * task's thread is pinned to one core, so sysconf says 1 on a machine
     * with twenty (worker.c's cores_online says the same). */
    if (!e) {
        unsigned cpus = 0;
        FILE *f = fopen("/sys/devices/system/cpu/online", "r");
        if (f) {
            unsigned a, b;
            int c;
            while (fscanf(f, "%u", &a) == 1) {
                b = a;
                if ((c = fgetc(f)) == '-') {
                    if (fscanf(f, "%u", &b) != 1)
                        break;
                    c = fgetc(f);
                }
                cpus += b >= a ? b - a + 1 : 0;
                if (c != ',')
                    break;
            }
            fclose(f);
        }
        if (!cpus) {
            long l = sysconf(_SC_NPROCESSORS_ONLN);
            cpus = l > 0 ? (unsigned)l : 0;
        }
        /* which one the tasks are on: this command runs on a task's thread,
         * pinned to it (runtime/task.c), and the hybrid list says its kind */
#ifdef __linux__
        int mine = sched_getcpu();
#else
        int mine = -1;                          /* hosted: no core of its own */
#endif
        char kind[32] = "";
        FILE *p = fopen("/sys/devices/cpu_core/cpus", "r");
        if (!p)
            p = fopen("/sys/devices/system/cpu/types/intel_core/cpulist", "r");
        if (p) {
            unsigned a, b;
            int c;
            while (fscanf(p, "%u", &a) == 1) {
                b = a;
                if ((c = fgetc(p)) == '-') {
                    if (fscanf(p, "%u", &b) != 1)
                        break;
                    c = fgetc(p);
                }
                if (mine >= (int)a && mine <= (int)b)
                    snprintf(kind, sizeof kind, ", a performance core");
                if (c != ',')
                    break;
            }
            fclose(p);
            if (!kind[0])
                snprintf(kind, sizeof kind, ", an efficiency core");
        }
        if (cpus && mine >= 0)
            e = print("Processors  %u: the tasks on cpu %d%s, %u for the workers\n",
                      cpus, mine, kind, cpus > 1 ? cpus - 1 : 1);
        else if (cpus)
            e = print("Processors  %u\n", cpus);
    }

#ifdef __linux__
    struct sysinfo si;
    if (!e && !sysinfo(&si))
        e = print("Memory      %lu MB, %lu MB free\n",
                  (unsigned long)(si.totalram * si.mem_unit >> 20),
                  (unsigned long)(si.freeram * si.mem_unit >> 20));
#endif

    /* What RISC OS itself has: the number its free pool is counted
     * against (OS_ReadMemMapInfo, capped at 2 GB unless rosgd.ram says
     * otherwise), and what is left of it. A task's slot and a dynamic
     * area are each their own mapping of the machine's memory. So this is
     * an allowance. It is neither the address space nor the machine's RAM. */
    if (!e) {
        uint32_t page = 0, pages = 0;
        xos_read_mem_map_info(&page, &pages);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 6;                             /* the free pool */
        ros_swi(&c, XOS_ReadDynamicArea);
        unsigned long long total = (unsigned long long)page * pages >> 20;
        if (!c.v)
            e = print("RISC OS     %llu MB to hand out, %lu MB of it free\n", total,
                      (unsigned long)(c.r[1] >> 20));
        else
            e = print("RISC OS     %llu MB to hand out\n", total);
    }

    if (!e) {
        int direct = 0;
        const char *drv = ros_display_driver(&direct);
        char name[64], mode[64];
        screen_name(name, sizeof name);
        screen_mode(mode, sizeof mode);
        e = print("Screen      %s, %s\n            %s, %s\n", name, mode, drv,
                  direct ? "scanning out the box's own screen memory"
                         : "given a copy of each frame");
    }

    struct ros_audio_output out[16];
    unsigned n = ros_audio_outputs(out, 16);
    if (!e) {
        const struct ros_audio_output *playing = NULL;
        for (unsigned i = 0; i < n; i++)
            if (out[i].playing)
                playing = &out[i];
        if (!playing)
            e = print("Sound       nothing: no card plays 16-bit stereo\n");
        else if (playing->how[0])
            e = print("Sound       %s on %s (card %u device %u of %u output%s)\n", playing->name,
                      playing->how, playing->card, playing->device, n, n == 1 ? "" : "s");
        else
            e = print("Sound       %s (card %u device %u of %u output%s)\n", playing->name,
                      playing->card, playing->device, n, n == 1 ? "" : "s");
    }

    struct ifaddrs *ifs = NULL;
    if (!e && !getifaddrs(&ifs)) {
        for (struct ifaddrs *a = ifs; a && !e; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET ||
                (a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP))
                continue;
            char text[INET_ADDRSTRLEN] = "";
            const struct sockaddr_in *in = (const struct sockaddr_in *)(void *)a->ifa_addr;
            inet_ntop(AF_INET, &in->sin_addr, text, sizeof text);
            e = print("Network     %s is %s\n", a->ifa_name, text);
        }
        freeifaddrs(ifs);
    }

    static const char *const discs[][2] = { { "/disc", "HostFS::Disc" }, { "/host", "HostFS::Host" } };
    for (unsigned i = 0; i < 2 && !e; i++) {
        struct statvfs v;
        if (statvfs(discs[i][0], &v) || !v.f_blocks)
            continue;
        unsigned long long total = (unsigned long long)v.f_blocks * v.f_frsize >> 20;
        unsigned long long free_ = (unsigned long long)v.f_bavail * v.f_frsize >> 20;
        e = print("Disc        %s: %llu MB, %llu MB free\n", discs[i][1], total, free_);
    }
    return e;
}

/* The kernel's own messages: each record is "<priority>,<sequence>,
 * <microseconds>,<flags>;<text>".  A new reader is given the ring from its
 * start, so this is the whole log, as far back as it goes. */
static os_error *cmd_kernellog(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char want[128] = "";
    const char *t = ros_ptr(tail);
    size_t n = 0;
    while (*t == ' ')
        t++;
    while ((uint8_t)t[n] >= ' ' && n < sizeof want - 1)
        want[n] = t[n], n++;
    want[n] = 0;

    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return ros_error(ERR_MACHINE, "The kernel keeps no log here (%s)", strerror(errno));
    os_error *e = NULL;
    unsigned shown = 0;
    for (;;) {
        char record[1024];
        ssize_t got = read(fd, record, sizeof record - 1);
        if (got < 0) {
            if (errno == EPIPE)         /* the ring overtook us: go on */
                continue;
            break;                      /* EAGAIN: the end of what there is */
        }
        record[got] = 0;
        char *text = strchr(record, ';');
        if (!text)
            continue;
        *text++ = 0;
        text[strcspn(text, "\n")] = 0;
        if (want[0] && !strstr(text, want))
            continue;
        unsigned long long us = 0;
        if (sscanf(record, "%*u,%*u,%llu", &us) != 1)
            us = 0;
        if ((e = print("[%5llu.%06llu] %s\n", us / 1000000, us % 1000000, text)) != NULL)
            break;
        shown++;
    }
    close(fd);
    if (!e && !shown)
        e = print(want[0] ? "No kernel message says \"%s\".\n" : "The kernel's log is empty.\n", want);
    return e;
}

static os_error *cmd_restart(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    print("RISC OS starting again.\n");
    ros_relaunch();
}

static os_error *cmd_reboot(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    print("Restarting.\n");
    ros_restart();
}

static os_error *cmd_poweroff(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    print("Stopping.\n");
    ros_poweroff(0);
}

static const struct ros_command commands[] = {
    { "MachineInfo", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *MachineInfo",
      "*MachineInfo says what the box is running on: the kernel, how long it has been\r"
      "up, its memory, what is driving the screen and in which mode, where the sound\r"
      "goes, the network addresses and the discs.\r",
      cmd_info },
    { "KernelLog", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *KernelLog [<text>]",
      "*KernelLog prints the messages the kernel under the box has written, oldest\r"
      "first, with the seconds since it started. With <text>, only the lines that\r"
      "have it: *KernelLog hda, *KernelLog error.\r",
      cmd_kernellog },
    { "Restart", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Restart",
      "*Restart starts RISC OS again on the Linux that is already running: a second or so,\r"
      "where *Reboot restarts the machine and goes through its firmware. It is the same\r"
      "system again -- a new one is on the EFI partition, which only *Reboot picks up.\r"
      "Everything running is ended first; files are written out.\r",
      cmd_restart },
    { "Reboot", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Reboot",
      "*Reboot restarts the machine, through its firmware. Files are written out first;\r"
      "nothing is asked. It is how a system written to the EFI partition from another\r"
      "machine is put into use; *Restart is the quick one, and keeps the system it has.\r",
      cmd_reboot },
    { "PowerOff", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *PowerOff",
      "*PowerOff stops the machine. Files are written out first; nothing is asked.\r",
      cmd_poweroff },
    { 0 },
};

struct ros_module machine_module = {
    .title = "Machine",
    .help = "Machine\t1.00 (02 Oct 2026) ROSGD native",
    .commands = commands,
};
