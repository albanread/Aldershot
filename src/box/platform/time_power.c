/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* time_power.c -- the clock, the kernel command line, and power off. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#else
#include <pthread.h>
#endif

#include "rosgd/platform.h"

/* The calling thread's name: prctl is Linux's and takes the thread, macOS
 * has pthread_setname_np, which takes the name alone and names the caller
 * too.  Anywhere else the box runs unnamed rather than not at all. */
void ros_thread_name(const char *name)
{
#if defined(__linux__)
    prctl(PR_SET_NAME, name);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    (void)name;
#endif
}

/* RISC OS's monotonic time counts centiseconds from power-on, and Linux's
 * CLOCK_MONOTONIC counts from boot: the same clock. (The zero-page
 * MetroGnome word counts the ticks that the ticker has run: ticker.h.) */
uint32_t ros_time_cs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 100u + (uint64_t)t.tv_nsec / 10000000u);
}

const char *ros_cmdline(void)
{
    static char line[4096];
    static int loaded;
    if (!loaded) {
        loaded = 1;
        FILE *f = fopen("/proc/cmdline", "r");
        if (f) {
            size_t n = fread(line, 1, sizeof line - 1, f);
            fclose(f);
            while (n && (line[n - 1] == '\n' || line[n - 1] == ' '))
                n--;
            line[n] = 0;
        }
    }
    return line;
}

/* The next parameter on a kernel command line, as Linux's own parser
 * (lib/cmdline.c's next_arg) splits it: spaces separate parameters except
 * in double quotes, so rosgd.run="Obey HostFS::Host.$.Go" is one, its
 * value without the quotes; a parameter quoted whole, "name=a b", is too.
 * -> past it, or NULL at the end (#20) */
static const char *next_param(const char *p, const char **name, size_t *nlen,
                              const char **val, size_t *vlen)
{
    while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;
    if (!*p)
        return NULL;
    int quoted = *p == '"', inq = quoted;
    const char *s = p + quoted, *eq = NULL, *e = s;
    for (; *e && (inq || (*e != ' ' && *e != '\t' && *e != '\n')); e++) {
        if (!eq && *e == '=')
            eq = e;
        if (*e == '"')
            inq = !inq;
    }
    const char *end = e;
    if (quoted && end > s && end[-1] == '"')
        end--;
    *name = s;
    *nlen = (size_t)((eq ? eq : end) - s);
    *val = NULL;
    *vlen = 0;
    if (eq) {
        const char *v = eq + 1, *ve = end;
        if (v < ve && *v == '"') {
            v++;
            if (ve > v && ve[-1] == '"')
                ve--;
        }
        *val = v;
        *vlen = (size_t)(ve - v);
    }
    return e;
}

int ros_cmdline_find(const char *line, const char *word, const char **val, size_t *vlen)
{
    size_t n = strlen(word), nl, vl;
    const char *name, *v, *p = line;
    while ((p = next_param(p, &name, &nl, &v, &vl))) {
        /* the name, or the whole of name=value (ros_cmdline_has("ip=dhcp")) */
        if ((nl == n && !memcmp(name, word, n)) ||
            (v && v + vl - name == (ptrdiff_t)n && !memcmp(name, word, n))) {
            if (val)
                *val = v, *vlen = v ? vl : 0;
            return 1;
        }
    }
    return 0;
}

int ros_cmdline_has(const char *word)
{
    return ros_cmdline_find(ros_cmdline(), word, NULL, NULL);
}

const char *ros_cmdline_value(const char *word)
{
    static char value[256];
    const char *line = ros_cmdline(), *p = line, *v;
    size_t n = strlen(word), k;
    /* the first word=value, as before: a bare word before it is passed over */
    const char *name;
    size_t nl;
    while ((p = next_param(p, &name, &nl, &v, &k))) {
        if (nl == n && !memcmp(name, word, n) && v) {
            if (k >= sizeof value)
                k = sizeof value - 1;
            memcpy(value, v, k);
            value[k] = 0;
            return value;
        }
    }
    return NULL;
}

void ros_poweroff(int status)
{
    ros_console_printf("rosgd: power off (%s)\n", status ? "failure" : "success");
#ifdef __linux__
    if (getpid() == 1) {
        sync();
        reboot(RB_POWER_OFF);       /* init may not exit: the kernel would panic */
        for (;;)
            pause();
    }
#endif
    exit(status);
}

#ifdef __linux__
/* The partition with that name in its GPT, as /init finds the RISC OS
 * disc: "/dev/sda1" for "EFI system partition" */
static int find_partition(const char *want, char *dev, size_t size)
{
    DIR *d = opendir("/sys/class/block");
    int found = 0;
    for (struct dirent *e; d && !found && (e = readdir(d));) {
        char path[300], line[160], name[64] = "", partname[80] = "";
        snprintf(path, sizeof path, "/sys/class/block/%s/uevent", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "DEVNAME=", 8))
                snprintf(name, sizeof name, "%s", line + 8);
            else if (!strncmp(line, "PARTNAME=", 9))
                snprintf(partname, sizeof partname, "%s", line + 9);
        }
        fclose(f);
        if (name[0] && !strcmp(partname, want)) {
            snprintf(dev, size, "/dev/%s", name);
            found = 1;
        }
    }
    if (d)
        closedir(d);
    return found;
}

/* The system on the EFI partition, loaded ready to jump to: kexec, so the
 * machine's firmware is not asked and a PC does not sit through its POST.
 * The kernel there is the whole box (the ROM is built into it), so this is
 * how a system written to that partition from another machine is put into
 * use.  0 when it is loaded, else -1 and the firmware does it. */
#define KEXEC_FILE_NO_INITRAMFS 0x00000004u
#ifndef LINUX_REBOOT_CMD_KEXEC
#define LINUX_REBOOT_CMD_KEXEC 0x45584543u       /* musl's sys/reboot.h has RB_*, not these */
#endif

static int kexec_the_esp(void)
{
    char dev[80];
    if (!find_partition("EFI system partition", dev, sizeof dev))
        return -1;
    mkdir("/boot", 0755);
    if (mount(dev, "/boot", "vfat", MS_RDONLY, "") != 0)
        return -1;
    int e = -1;
    int fd = open("/boot/EFI/BOOT/BOOTX64.EFI", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char cmdline[4096] = "";
        FILE *c = fopen("/proc/cmdline", "r");
        if (c) {
            size_t n = fread(cmdline, 1, sizeof cmdline - 1, c);
            while (n && (cmdline[n - 1] == '\n' || cmdline[n - 1] == ' '))
                n--;
            cmdline[n] = 0;
            fclose(c);
        }
        e = (int)syscall(SYS_kexec_file_load, fd, -1, strlen(cmdline) + 1, cmdline,
                         KEXEC_FILE_NO_INITRAMFS);
        close(fd);
    }
    umount("/boot");
    return e == 0 ? 0 : -1;
}
#endif

void ros_restart(void)
{
#ifdef __linux__
    if (getpid() == 1) {
        sync();
        if (kexec_the_esp() == 0) {
            ros_console_printf("rosgd: restart, into the system on the EFI partition "
                               "(no firmware)\n");
            reboot(LINUX_REBOOT_CMD_KEXEC);
        }
        ros_console_printf("rosgd: restart, through the machine's firmware\n");
        reboot(RB_AUTOBOOT);
        for (;;)
            pause();
    }
#endif
    ros_console_printf("rosgd: restart\n");
    exit(0);
}

/* Start RISC OS again without restarting the machine.
 *
 * The box is /init, so it runs itself again. The kernel never stops, the
 * firmware is not asked, nothing is probed, and RISC OS comes back in about
 * a second. This is what a reset does on real hardware, and what a PC's
 * own restart cannot do without going through its BIOS. It is the same
 * system either way. The ROM is inside the kernel image, so a *new* one
 * still wants the machine restarted (*Reboot).
 *
 * What has to go first: every other process (sshd, task windows, whatever
 * *RunBox left), since exec keeps them and the next instance would find its
 * port and its devices taken. Every handle above the console goes too,
 * since a card or a display left open refuses the next instance. Mounts
 * stay. The disc is still at /disc, and mounting it again says EBUSY,
 * which the next instance takes for "it is already there". */
__attribute__((noreturn)) void ros_relaunch(void)
{
#ifdef __linux__
    if (getpid() == 1) {
        ros_console_printf("rosgd: RISC OS starting again\n");
        sync();
        kill(-1, SIGTERM);
        usleep(200000);
        kill(-1, SIGKILL);
        DIR *d = opendir("/proc/self/fd");
        if (d) {
            int self = dirfd(d);
            for (struct dirent *e; (e = readdir(d)) != NULL;) {
                int fd = atoi(e->d_name);
                if (fd > 2 && fd != self)
                    close(fd);
            }
            closedir(d);
        }
        execl("/init", "/init", (char *)NULL);
        ros_console_printf("rosgd: cannot start again (%s): stopping\n", strerror(errno));
    }
#endif
    ros_poweroff(1);
}

/* The HAL's counters (HAL_SYS_STATS, &7203): Linux has no such call and
 * answers ENOSYS, as it does the HAL's screen calls */
int ros_hal_stats(struct ros_hal_stats *s)
{
#ifdef __linux__
    memset(s, 0, sizeof *s);
    long n = syscall(0x7203, s, sizeof *s);
    return n > 0 && s->version >= 1 ? 0 : -1;
#else
    (void)s;
    return -1;
#endif
}
