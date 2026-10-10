/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* main.c: /init, the ROSGD runtime as a Linux box's first process.
 *
 * The kernel runs this from the initramfs, which is the ROM. It mounts what
 * Linux needs and the host's share, lays out RISC OS's memory map, places
 * the ROM and starts its modules. DRMVideo, the GraphicsV driver, opens the
 * display. FileSwitch finds the share, which HostFS presents as the disc
 * Host. /init then opens the input devices and enters the configured
 * language, which is the Desktop, as RISC OS starts. When that ends it gives
 * a * prompt.
 *
 * These words on the kernel command line change what it does:
 *
 *   rosgd.prompt     give the prompt at once, after the RISC OS disc's !Boot
 *                    if there is a disc.
 *   rosgd.disc       wait for the RISC OS disc to appear.
 *   rosgd.poweroff   run the self-test and power off. Automated runs end
 *                    this way.
 *   rosgd.selftest   run the self-test and then give the prompt.
 *   rosgd.run=<command>
 *                    run one command in place of the self-test, then power
 *                    off. This is the box's form of the hosted build's
 *                    ROSGD_CLI, used by tests/basic.
 *   rosgd.boot       run the share's !Boot (HostFS::Host.$.!Boot) first, as
 *                    RISC OS runs the boot disc's. It sets Choices$Write and
 *                    Wimp$Scrap, which the desktop's applications need. The
 *                    run scripts give it for a box in a window. The
 *                    self-test and the probes go without it.
 *   rosgd.sshd       start the SSH server (*SSHD) before the prompt. When
 *                    /init is run as riscos-cli, which is sshd's login
 *                    shell for root, it is not the box at all. It is the
 *                    relay from an SSH session to a RISC OS command line in
 *                    the running /init.
 *   rosgd.switrace[=all]
 *                    trace every SWI live on the console, as ROSGD_SWITRACE
 *                    does in the hosted build (include/rosgd/switrace.h).
 *   rosgd.capptest   switch on the C applications' test hooks
 *                    (boot/selftest.h) for what rosgd.run runs.
 *
 * A fault in a program is a RISC OS error. That covers x32 code and a SWI
 * given a bad pointer. It is reported as "rosgd: exception: ..."
 * (runtime/fault.c). A fault that /init does not handle is a signal that
 * nothing turns into a RISC OS error. It is reported on the console as
 * "rosgd: fault: ..." with the last SWIs, and the box powers off with a
 * failure status. tests/lib/boxrun.py then fails the run with the report.
 * If /init simply died, the kernel would panic instead.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "drmvideo.h"
#include "input.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/fault.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/rom.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/task.h"
#include "rosgd/ticker.h"
#include "rosgd/vector.h"
#include "selftest.h"
#include "sshd.h"
#include "rosgd/streams.h"

/* The slot pool (arena.c) is a tmpfs for application slots. It is as big as
 * the memory free now, less a reserve for what is not in a slot: the RMA,
 * the dynamic areas, the screen and the kernel's own memory. A program that
 * touches more than the pool holds gets a data abort, and the box lives on.
 * The reserve is 256 MB, or an eighth of the memory if that is more. */
static void mount_slot_pool(void)
{
    uint64_t total = 0, avail = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        unsigned long long kb;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
                total = kb << 10;
            else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
                avail = kb << 10;
        }
        fclose(f);
    }
    uint64_t reserve = total / 8 > (256u << 20) ? total / 8 : (256u << 20);
    if (avail <= reserve + (64u << 20))
        return;                         /* too little to pool: slots are memfds */
    char opt[64];
    snprintf(opt, sizeof opt, "size=%llu,mode=0700", (unsigned long long)((avail - reserve) >> 12 << 12));
    mkdir("/run", 0755);
    mkdir(ROS_SLOT_POOL, 0700);
    mount("slots", ROS_SLOT_POOL, "tmpfs", MS_NOSUID | MS_NODEV, opt);
}

static void mount_linux(void)
{
    mkdir("/dev", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);
    mkdir("/sys", 0555);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount_slot_pool();
    /* ScratchSpace is at &4000 and DebuggerSpace at &2000, below
     * application space. A kernel built before this was allowed maps
     * nothing under &8000 (config fragment). The limit is set to one page.
     * Page zero stays unmappable, so a null pointer followed in the box's
     * own compiled code still faults where it is made. The compatibility
     * page that RISC OS can put at &0 stays off here (dynarea.c, area 16). */
    FILE *mm = fopen("/proc/sys/vm/mmap_min_addr", "w");
    if (mm) {
        fputs("4096\n", mm);
        fclose(mm);
    }
    /* The kernel logs every SIGSEGV of init ("init[1]: segfault at ..."),
     * whether or not init has handlers. Here a program's faults are RISC OS
     * errors (runtime/fault.c), and /init reports for itself any fault it
     * cannot handle, because its death would panic the kernel. So the log
     * lines are switched off. The switch covers the whole system. Other
     * processes' lines (KERN_INFO) go too, but under the box's "quiet" they
     * never reached the console. */
    FILE *et = fopen("/proc/sys/debug/exception-trace", "w");
    if (et) {
        fputs("0\n", et);
        fclose(et);
    }
    if (fcntl(1, F_GETFD) < 0) {        /* no console from the kernel */
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
    }
}

/* The host's share, when the VM has one, is the outside half of the one
 * filing system: HostFS over Linux's. QEMU exports a host directory over
 * 9p with the mount tag "host". On Apple Silicon, rosgd-vz exports it over
 * virtio-fs with the same tag. QEMU on a Mac (run/run-qemu.sh) has no
 * virtio-fs daemon, so it uses 9p again. For that reason virtio-fs is tried
 * first and 9p second. */
/* Once a second, forget the entries of the cached 9p share that are not in
 * use. What the Mac changes there is then seen within a second, as on VZ.
 * Linux's 9p revalidates nothing it has cached, and the only way to expire
 * the entries is to drop the reclaimable dentries and inodes with their
 * pages. Open files and data not yet written back are kept. */
static void *fresh_9p_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (write(fd, "2", 1) < 0) { }
            close(fd);
        }
    }
    return NULL;
}

static void fresh_9p(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fresh_9p_thread, NULL) == 0)
        pthread_detach(t);
}

static int mount_host_share(void)
{
    mkdir("/host", 0755);
    int ok = 0;
#if defined(__aarch64__)
    ok = mount("host", "/host", "virtiofs", 0, NULL) == 0;
#endif
    if (!ok) {
        /* 9p caches nothing by itself. Over QEMU on a Mac each of its
         * messages costs a few hundred microseconds. A catalogue of 300
         * files took 340 ms, against 12 ms on VZ's virtio-fs (#144). So the
         * share's metadata and pages are cached (cache=0x7 is CACHE_META,
         * CACHE_FILE and CACHE_WRITEBACK) and made fresh every second
         * (fresh_9p), as virtio-fs's one-second attribute timeouts make
         * them. HostFS flushes a file it wrote when it closes it.
         * rosgd.9pcache=MODE sets the mode instead. The mode "none" gives
         * the old behaviour. */
        const char *cache = ros_cmdline_value("rosgd.9pcache");
        char opts[160];
        snprintf(opts, sizeof opts, "trans=virtio,version=9p2000.L,msize=262144,cache=%s",
                 cache ? cache : "0x7");
        ok = mount("host", "/host", "9p", 0, opts) == 0;
        if (!ok && cache)
            ros_console_printf("rosgd: 9p %s: %s\n", opts, strerror(errno));
        if (ok && !(cache && !strcmp(cache, "none")))
            fresh_9p();
    }
    if (!ok) {
        ros_console_printf("rosgd: no host share: %s\n", strerror(errno));
        return 0;
    }
    unsigned entries = 0;
    DIR *d = opendir("/host");
    for (struct dirent *e; d && (e = readdir(d));)
        entries += e->d_name[0] != '.';
    if (d)
        closedir(d);
    ros_console_printf("rosgd: host share: /host, %u %s\n", entries,
                       entries == 1 ? "entry" : "entries");

    char line[128] = "";
    FILE *f = fopen("/host/hello.txt", "r");
    if (f) {
        if (fgets(line, sizeof line, f))
            line[strcspn(line, "\n")] = 0;
        fclose(f);
        ros_console_printf("rosgd: host share: hello.txt says \"%s\"\n", line);
    }
    return 1;
}

/* The RISC OS disc is the partition named "RISC OS disc" in the GPT. It
 * holds an ext4 file system, which is mounted at /disc. FileSwitch makes it
 * HostFS's disc Disc, where files outlive the box. A PC's image carries one
 * (image/README.md). With rosgd.disc, /init waits up to ten seconds for it,
 * because a USB stick's partitions appear a moment after the kernel starts
 * /init. */
static int find_disc(char *dev, size_t size)
{
    DIR *d = opendir("/sys/class/block");
    int found = 0;
    for (struct dirent *e; d && !found && (e = readdir(d));) {
        char path[300], line[128], name[64] = "", partname[64] = "";
        snprintf(path, sizeof path, "/sys/class/block/%s/uevent", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (strncmp(line, "DEVNAME=", 8) == 0)
                snprintf(name, sizeof name, "%s", line + 8);
            else if (strncmp(line, "PARTNAME=", 9) == 0)
                snprintf(partname, sizeof partname, "%s", line + 9);
        }
        fclose(f);
        if (name[0] && strcmp(partname, "RISC OS disc") == 0) {
            snprintf(dev, size, "/dev/%s", name);
            found = 1;
        }
    }
    if (d)
        closedir(d);
    return found;
}

static void mount_disc(void)
{
    char dev[80];
    int tries = ros_cmdline_has("rosgd.disc") ? 100 : 1;
    int found = 0;
    for (int i = 0; i < tries && !(found = find_disc(dev, sizeof dev)); i++)
        usleep(100000);
    if (!found) {
        if (tries > 1)
            ros_console_printf("rosgd: no RISC OS disc (a partition named \"RISC OS disc\")\n");
        return;
    }
    mkdir("/disc", 0755);
    if (mount(dev, "/disc", "ext4", MS_NOATIME, "") != 0) {
        ros_console_printf("rosgd: RISC OS disc %s: %s\n", dev, strerror(errno));
        rmdir("/disc");
        return;
    }
    struct statvfs v;
    if (statvfs("/disc", &v) == 0)
        ros_console_printf("rosgd: RISC OS disc: %s, ext4, %llu of %llu MB free\n", dev,
                           (unsigned long long)v.f_bavail * v.f_frsize >> 20,
                           (unsigned long long)v.f_blocks * v.f_frsize >> 20);
}

/* This sets up what a Linux program expects of the system. An example is
 * *SSH's ssh, on the PTY module's terminal. It needs pseudo-terminals
 * (devpts), a password file naming root and its home, and /tmp. The home is
 * the host's share when there is one, so that ~/.ssh outlives the box. That
 * directory holds the box's host key, the keys that may log in and the known
 * hosts. It is $./ssh on HostFS::Host. A PC has no share. There the RISC OS
 * disc is the home, and the same files live on the disc it booted from
 * ($./ssh on HostFS::Disc). */
static void unix_setup(int share)
{
    struct stat ds;
    const char *home = share                                     ? "/host"
                       : stat("/disc", &ds) == 0 && S_ISDIR(ds.st_mode) ? "/disc"
                                                                 : "/root";
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, "mode=0620,ptmxmode=0666");
    mkdir("/tmp", 01777);
    chmod("/tmp", 01777);
    mkdir("/root", 0700);
    mkdir("/etc", 0755);
    FILE *f = fopen("/etc/passwd", "w");
    if (f) {
        fprintf(f, "root:x:0:0:root:%s:/usr/bin/riscos-cli\n", home);
        fputs("sshd:x:74:74:sshd privilege separation:/var/empty:/bin/false\n", f);
        fputs("nobody:x:65534:65534:SMB guests:/var/empty:/bin/false\n", f);
        fclose(f);
    }
    if ((f = fopen("/etc/group", "w"))) {
        fputs("root:x:0:\nsshd:x:74:\nnogroup:x:65534:\n", f);
        fclose(f);
    }
    /* root's shell is /init itself. Run as riscos-cli, it joins an SSH
     * session to a RISC OS command line (modules/sshd). */
    mkdir("/usr", 0755);
    mkdir("/usr/bin", 0755);
    symlink("/init", "/usr/bin/riscos-cli");
}

/* Name on the console the addresses that the box answers on. A machine
 * managed over SSH is reached by one of them, and its user reads them on
 * its screen (rosgd.sshd). */
static void report_addresses(void)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0)
        return;
    for (struct ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET ||
            !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK))
            continue;
        char dotted[INET_ADDRSTRLEN] = "";
        const struct sockaddr_in *in = (const struct sockaddr_in *)a->ifa_addr;
        if (inet_ntop(AF_INET, &in->sin_addr, dotted, sizeof dotted))
            ros_console_printf("rosgd: network: %s is %s\n", a->ifa_name, dotted);
    }
    freeifaddrs(list);
}

static void fill(const struct ros_display *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t colour)
{
    for (uint32_t row = y; row < y + h && row < d->height; row++) {
        uint32_t *p = ros_ptr(d->base + row * d->stride + x * 4);
        for (uint32_t col = x; col < x + w && col < d->width; col++)
            *p++ = colour;
    }
}

/* Show the self-test's result on screen, written as screen memory. A band
 * is green when everything passed. There is one block for each check. */
static void show(const struct ros_display *d, const struct ros_selftest *t)
{
    uint32_t good = ros_rgb(0x2E, 0xA0, 0x43), bad = ros_rgb(0xD0, 0x30, 0x30);
    fill(d, 0, 0, d->width, d->height, ros_rgb(0xDD, 0xDD, 0xDD));
    fill(d, 0, 0, d->width, 64, t->failed ? bad : good);
    for (unsigned i = 0; i < t->count && i < sizeof t->ok; i++)
        fill(d, 32 + (i % 16) * 40, 96 + (i / 16) * 40, 32, 32, t->ok[i] ? good : bad);
    ros_display_update(d);
}

static void prompt(void);
static void language(void);
static void disc_boot(void);
static void share_boot(void);
static void cli(const char *command);

/* The fault report (see the top of the file). The handler runs on a stack
 * of its own, so that an overflowed stack is reported too. It says only what
 * it can without the runtime's help: the signal, the place it happened, the
 * address and the task. The place is also given as an offset in /init, which
 * the test harness turns into a name with build/init. */
extern const char __ehdr_start[] __attribute__((weak));
static volatile int arena_up;           /* zero page there to read the task from */

static void report_fault(int sig, siginfo_t *si, void *context)
{
    static const char *const names[] = {
        [SIGSEGV] = "SIGSEGV", [SIGBUS] = "SIGBUS", [SIGILL] = "SIGILL",
        [SIGFPE] = "SIGFPE", [SIGABRT] = "SIGABRT", [SIGSYS] = "SIGSYS", [SIGTRAP] = "SIGTRAP",
    };
    uintptr_t pc = 0, addr = (uintptr_t)si->si_addr;
#if defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)context)->uc_mcontext.pc;
#else
    (void)context;
#endif
    char line[240], in[40] = "", where[40] = "";
    uint32_t task = arena_up ? ros_ld32(ROS_ZP_DOMAINID) : 0;
    uintptr_t image = (uintptr_t)__ehdr_start;
    if (image && pc >= image && pc - image < ((uintptr_t)1 << 30))
        snprintf(in, sizeof in, " (/init+%#lx)", (unsigned long)(pc - image));
    if (sig != SIGABRT && addr < ((uintptr_t)1 << 32))
        snprintf(where, sizeof where, " (arena &%08X)", (unsigned)addr);
    int n = snprintf(line, sizeof line,
                     "\nrosgd: fault: %s, code %d, at pc %#lx%s, address %#lx%s, task &%X\n",
                     sig < (int)(sizeof names / sizeof *names) && names[sig] ? names[sig] : "signal",
                     si->si_code, (unsigned long)pc, in, (unsigned long)addr, where, task);
    write(2, line, (size_t)n);
    fputs("rosgd: fault: the last SWIs:\n", stderr);
    ros_switrace_dump(stderr, 24);
    fflush(stderr);
    ros_poweroff(1);
}

static void catch_faults(void)
{
    static char stack[64 * 1024];
    stack_t ss = {.ss_sp = stack, .ss_size = sizeof stack};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {.sa_sigaction = report_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++)
        sigaction(sigs[i], &sa, NULL);
    /* A program's faults become RISC OS errors. That covers x32 code and a
     * SWI given a bad pointer. The rest come back to report_fault
     * (runtime/fault.c). */
    ros_fault_init(report_fault);
}

int main(int argc, char **argv)
{
    const char *self = argc > 0 && argv[0] ? argv[0] : "";
    const char *me = strrchr(self, '/');
    me = me ? me + 1 : self;
    if (!strcmp(me + (*me == '-'), "riscos-cli"))     /* sshd's login shell ("-" a login) */
        return sshd_relay(argc, argv);
    if (getpid() == 1)
        mount_linux();
    ros_console_init();
    catch_faults();

    struct utsname u;
    uname(&u);
    /* The kernel's name: HybrisOS (the HAL, whose uname says "BOX") or Linux. */
    const char *kernel = strcmp(u.sysname, "BOX") == 0 ? "HybrisOS" : "Linux";
    ros_console_printf("\nROSGD 0.01 -- a RISC OS personality on %s %s (%s)\n", kernel, u.release,
                       u.machine);

    int e = ros_arena_init();
    if (e) {
        ros_console_printf("rosgd: cannot lay out the arena: %s\n", strerror(-e));
        ros_poweroff(1);
    }
    arena_up = 1;
    ros_console_printf("rosgd: arena: application &%08X, RMA &%08X, ROM &%08X, zero page &%08X\n",
                       ROS_APP_BASE, ROS_RMA_BASE, ROS_ROM_BASE, ROS_ZEROPAGE);
    if (ros_slot_pool_free() != UINT64_MAX)
        ros_console_printf("rosgd: slots: up to %u MB each, lazy; the slot pool %llu MB\n",
                           (ROS_APP_LIMIT - ROS_APP_BASE) >> 20,
                           (unsigned long long)(ros_slot_pool_free() >> 20));

    /* /init is the one task. It holds the personality lock from here on,
     * and lets it go only to wait (background.h). */
    ros_lock();
    if ((e = ros_background_start()) || (e = ros_ticker_start()) || (e = ros_tasks_init()))
        ros_console_printf("rosgd: no background thread: %s\n", strerror(-e));
    ros_swi_init();
    if (getpid() == 1)
        mount_disc();                   /* as the share, before FileSwitch */
    if (getpid() == 1)
        unix_setup(mount_host_share()); /* before FileSwitch looks for it */
    os_error *err = ros_rom_init();
    if (err) {
        ros_console_printf("rosgd: ROM: %s\n", err->errmess);
        ros_poweroff(1);
    }
    ros_callbacks_run();                /* Service_BufferStarting, and the like */

    ros_audio_init();                  /* silence until a driver starts */

    const struct ros_display *display = drmvideo_display();
    if (display)
        ros_console_printf("rosgd: display: %ux%u, %u bpp, %s, screen memory at &%08X, "
                           "modes up to %ux%u\n", display->width, display->height,
                           display->bpp, display->name, display->base, display->max_width,
                           display->max_height);
    else
        ros_console_printf("rosgd: no display\n");
    ros_console_printf("rosgd: input: %d device(s)\n", input_device_count());
    int up = ros_net_init();
    ros_console_printf("rosgd: network: %d IPv4 address(es) up%s\n", up < 0 ? 0 : up,
                       ros_cmdline_has("ip=dhcp") ? ", configured by the kernel's DHCP" : "");
    report_addresses();

    /* When asked, run the HostFS suite's executor instead of the self-test
     * (tests/hostfs/rosgd_suite.py). */
    if (ros_cmdline_has("rosgd.hfstest")) {
        ros_hfstest_serve("HostFS:$.HFT");
        ros_poweroff(0);
    }

    /* When asked, switch on the C applications' test hooks, for probes run
     * by rosgd.run (tests/capps/baton.py). They stand in for SharedCLibrary's
     * calls. */
    if (ros_cmdline_has("rosgd.capptest"))
        ros_selftest_capps_hooks(1);

    /* Set what RISC OS 5's !Boot sets first (its BootVars) and what
     * applications test. This is the OS version, which is "530" for 5.30.
     * !OvnPro's !Words chooses its icon sprites by it, and below 500 it names
     * a file that is not there. */
    cli("Set Boot$OSVersion 530");
    /* The kernel the box runs on, by name, for the disc's !Boot to say. */
    cli(strcmp(kernel, "HybrisOS") == 0 ? "Set BOX$Kernel HybrisOS" : "Set BOX$Kernel Linux");
    /* Set !System's variables too. Stock !Run files stop with "System
     * resources cannot be found" when System$Path is unset. The modules they
     * would load from it are the ROM's (resources/Resources/!System). */
    cli("Set System$Dir Resources:$.Resources.!System");
    cli("Set System$Path <System$Dir>.");

    /* Run the share's !Boot, for a box started to be used (rosgd.boot), and
     * the RISC OS disc's. A PC boots from the disc alone, and there the
     * disc's !Boot sets Choices$Write and Wimp$Scrap (image/README.md). */
    if (ros_cmdline_has("rosgd.boot")) {
        share_boot();
        disc_boot();
    }

    /* Run one command instead of the self-test, as the hosted build's
     * ROSGD_CLI does. An example is rosgd.run=HostFS::Host.$.BT.Run
     * (tests/basic/compare.py). */
    const char *run = ros_cmdline_value("rosgd.run");
    if (run) {
        char *cmd = ros_rma_alloc(256);
        snprintf(cmd, 256, "%s\r", run);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = ros_addr(cmd);
        ros_swi(&s, XOS_CLI);
        if (s.v)
            ros_console_printf("rosgd: %s\n", ((os_error *)ros_ptr(s.r[0]))->errmess);
        ros_poweroff(s.v ? 1 : 0);
    }

    /* Run the self-test for make boot (rosgd.poweroff, which ends the run)
     * or when asked for (rosgd.selftest). A box started to be used goes to
     * its configured language, as RISC OS does. */
    if (ros_cmdline_has("rosgd.poweroff") || ros_cmdline_has("rosgd.selftest")) {
        struct ros_selftest t;
        int ok = ros_selftest(&t);
        if (display)
            show(display, &t);
        if (display && ros_cmdline_has("rosgd.gvdemo"))
            ros_graphicsv_demo();
        if (ros_cmdline_has("rosgd.sine"))
            ros_audio_sine();
        if (ros_cmdline_has("rosgd.poweroff"))
            ros_poweroff(ok ? 0 : 1);
    }

    if (ros_cmdline_has("rosgd.sshd")) {
        char *cmd = ros_rma_alloc(16);
        memcpy(cmd, "SSHD\r", 6);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = ros_addr(cmd);
        ros_swi(&s, XOS_CLI);
        if (s.v)
            ros_console_printf("rosgd: %s\n", ((os_error *)ros_ptr(s.r[0]))->errmess);
        ros_rma_free(cmd);
    }

    if (!ros_cmdline_has("rosgd.prompt") && !ros_cmdline_has("rosgd.selftest"))
        language();
    else if (ros_cmdline_has("rosgd.prompt"))
        disc_boot();
    ros_console_printf("rosgd: ready.  At the * prompt, RISC OS commands -- *Show, *Set, "
                       "*Help Set ...; Ctrl-D powers off.  With rosgd.inputlog, keys and the "
                       "pointer are reported here.\n");
    prompt();
}

/* Enter the configured language, as the kernel's reset does at its end
 * (Kernel/s/NewReset). CMOS Language (&B9) is a module's position in the
 * ROM. It uses the numbering of RISC OS 5.30's Pi ROM, which the emulator's
 * CMOS and a 5.30 machine share. In that numbering 11 is the Desktop, the
 * default. The title at that position (pi_rom, as *ROMModules lists it
 * there) is entered if ROSGD has the module. The * prompt is used instead
 * if keypad-* is held, if ROSGD has no module at that position, or if the
 * module will not enter. The kernel falls back to the Supervisor in the
 * same way. When the language ends, because the desktop quit, so does this
 * function. */
static const char *const pi_rom[] = {
    "UtilityModule", "PCI", "FileSwitch", "ResourceFS", "TerritoryManager", "Messages",
    "MessageTrans", "UK", "WindowManager", "TaskManager", "Desktop", "SharedCLibrary",
    "BASIC", "BASIC64", "BASICVFP", "BlendTable", "BufferManager", "ColourTrans",
    "Debugger", "DeviceFS", "BCMSupport", "Portable", "RTSupport", "USBDriver", "DWCDriver",
    "XHCIDriver", "VCHIQ", "BCMSound", "ScreenModes", "BCMVideo", "DisplayManager",
    "DMAManager", "DragASprite", "DragAnObject", "Draw", "BBCEconet", "FileCore", "RamFS",
    "Filer", "FilerSWIs", "FSLock", "FontManager", "FPEmulator", "VFPSupport", "Free",
    "Hourglass", "IIC", "International", "InternationalKeyboard", "InverseTable", "NetFS",
    "NetFiler", "NetPrint", "NetStatus", "Obey", "Pinboard", "PipeFS", "RAMFSFiler",
    "ResourceFiler", "ROMFonts", "RTC", "ScreenBlanker", "ScrSaver", "Serial",
    "SerialDeviceSupport", "ShellCLI", "SoundDMA", "SoundControl", "SoundChannels",
    "SoundScheduler", "SpriteExtend", "SpriteUtils", "Squash", "BootFX", "SuperSample",
    "SystemDevices", "TaskWindow", "WindowUtils", "FilterManager", "WaveSynth", "StringLib",
    "Percussion", "SharedSound", "Filer_Action", "DOSFS", "SCSIDriver", "SCSISoftUSB",
    "SCSIFS", "SCSIFiler", "SDIODriver", "SDFS", "SDFSFiler", "SDCMOS", "ColourPicker",
    "DrawFile", "BootCommands", "WindowScroll", "MbufManager", "Internet", "Resolver",
    "Net", "BootNet", "Freeway", "ShareFS", "MimeMap", "LanManFS", "EtherGENET", "EtherUSB",
    "DHCP", "!Edit", "!Draw", "!Paint", "!Alarm", "!Chars", "!Help", "Toolbox", "Window",
    "ToolAction", "Menu", "Iconbar", "ColourDbox", "ColourMenu", "DCS", "FileInfo",
    "FontDbox", "FontMenu", "PrintDbox", "ProgInfo", "SaveAs", "Scale", "TextGadgets",
    "CDFSDriver", "CDFSSoftSCSI", "CDFS", "CDFSFiler", "UnSqueezeAIF", "GPIO", "HostFS",
    "HostFSFiler"
};

static void language(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 129, s.r[1] = 0xA4, s.r[2] = 0xFF;     /* INKEY -92: keypad-* */
    ros_swi(&s, XOS_Byte);
    if (!s.v && s.r[1] == 0xFF && s.r[2] == 0xFF)
        return;
    ros_cpu_enter(&s);
    s.r[0] = 161, s.r[1] = 0xB9;                    /* LanguageCMOS */
    ros_swi(&s, XOS_Byte);
    uint32_t n = s.r[2];
    if (s.v || n == 0 || n > sizeof pi_rom / sizeof pi_rom[0])
        return;
    char *title = ros_rma_alloc(64);
    if (!title)
        return;
    snprintf(title, 64, "%s", pi_rom[n - 1]);
    ros_cpu_enter(&s);
    s.r[0] = 18, s.r[1] = ros_addr(title);          /* OS_Module 18: look it up */
    ros_swi(&s, XOS_Module);
    if (!s.v) {
        /* Set the desktop's wallpaper, for a box started to be used.
         * run/run-x86_64.sh gives its windows rosgd.backdrop. The command
         * is passed as *Desktop's own <*command>, which it starts with
         * Wimp_StartTask once the desktop is up, along with Pinboard's task.
         * It is *Backdrop -Tile with ROSGD's grey tile
         * (tools/desktopart.py) or the file named. The probes' boxes keep
         * RISC OS's plain grey, as 5.30 has it with no !Boot. */
        char *tail = ros_rma_alloc(300);
        if (tail) {
            tail[0] = 13;
            if (ros_cmdline_has("rosgd.backdrop")) {
                const char *file = ros_cmdline_value("rosgd.backdrop");
                snprintf(tail, 300, "Backdrop -Tile %s\r",
                         file && *file ? file : "Resources:$.Resources.Pinboard.Tile");
            }
            ros_cpu_enter(&s);
            s.r[0] = 2, s.r[1] = ros_addr(title), s.r[2] = ros_addr(tail);   /* enter it */
            ros_swi(&s, XOS_Module);
            if (s.v)
                ros_console_printf("rosgd: %s\n", ((os_error *)ros_ptr(s.r[0]))->errmess);
            ros_rma_free(tail);
        }
    }
    ros_rma_free(title);
}

/* Run one command line as the prompt runs it, and write out any error. */
static void cli(const char *command)
{
    char *line = ros_rma_alloc(256);
    if (!line)
        return;
    snprintf(line, 256, "%s\r", command);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(line);
    ros_swi(&s, XOS_CLI);
    if (s.v) {
        xos_write0(((os_error *)ros_ptr(s.r[0]))->errmess, NULL);
        xos_new_line();
    }
    ros_rma_free(line);
}

/* Before the prompt, with a RISC OS disc, make the disc the current
 * directory and run its !Boot, as RISC OS runs the boot disc's (*Opt 4,2).
 * This is the disc's own start-up, which the user can change. */
static void disc_boot(void)
{
    static int done;
    struct stat st;
    if (done || stat("/disc", &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    done = 1;
    cli("Dir HostFS::Disc.$");
    char *name = ros_rma_alloc(32);
    if (!name)
        return;
    snprintf(name, 32, "HostFS::Disc.$.!Boot");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 17, s.r[1] = ros_addr(name);          /* OS_File 17: is it there? */
    ros_swi(&s, XOS_File);
    int there = !s.v && s.r[0] != 0;
    ros_rma_free(name);
    if (there)
        cli("Run HostFS::Disc.$.!Boot");
}

/* With rosgd.boot, before the desktop, run the share's !Boot, as RISC OS
 * runs the boot disc's. It sets Choices$Write, Choices$Path, Wimp$ScrapDir
 * and Wimp$Scrap (disc/!Boot). On RISC OS !Boot sets these, and the ROM
 * never does. If there is no !Boot on the share, nothing happens. */
static void share_boot(void)
{
    struct stat st;
    if (stat("/host", &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    char *name = ros_rma_alloc(32);
    if (!name)
        return;
    snprintf(name, 32, "HostFS::Host.$.!Boot");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 17, s.r[1] = ros_addr(name);          /* OS_File 17: is it there? */
    ros_swi(&s, XOS_File);
    int there = !s.v && s.r[0] != 0;
    ros_rma_free(name);
    if (there)
        cli("Run HostFS::Host.$.!Boot");
}

/* A * prompt. A line is read and edited with Delete, then run by OS_CLI as a
 * RISC OS command line is, and any error is reported. Keys come through
 * OS_ReadC, from the box's keyboard or from this console, which is a
 * keyboard too. While OS_ReadC waits, it lets SSH sessions' command lines
 * run. */
static void prompt(void)
{
    char *line = ros_rma_alloc(256);
    unsigned n = 0;
    xos_write_c('*');
    for (;;) {
        uint8_t ch;
        int escape = 0;
        xos_read_c(&ch, &escape);
        ros_callbacks_run();
        if (escape) {                               /* acknowledged, the line dropped */
            struct ros_cpu b;
            ros_cpu_enter(&b);
            b.r[0] = 126;
            ros_swi(&b, XOS_Byte);
            xos_new_line();
            xos_write0("Escape", NULL);
            xos_new_line();
            n = 0;
            xos_write_c('*');
            continue;
        }
        int c = ch;
        if (c == 4)
            ros_poweroff(0);
        if (c == 13 || c == 10) {
            xos_new_line();
            line[n] = 13;
            struct ros_cpu s;
            ros_cpu_enter(&s);
            s.r[0] = ros_addr(line);
            ros_swi(&s, XOS_CLI);
            if (s.v) {
                xos_write0(((os_error *)ros_ptr(s.r[0]))->errmess, NULL);
                xos_new_line();
            }
            n = 0;
            xos_write_c('*');
        } else if ((c == 8 || c == 127) && n) {
            n--;
            xos_write_c(8), xos_write_c(' '), xos_write_c(8);
        } else if (c >= ' ' && c != 127 && n < 255) {
            line[n++] = (char)c;
            xos_write_c((uint8_t)c);
        }
    }
}
