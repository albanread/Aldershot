/* wperun.c -- run a program of the WPE edition's Linux root (design 23,
 * section 10): the 64-bit Debian system ports/wpe/mkwpe.py makes, on a
 * second disk.
 *
 *   wperun [-d] <program> [arguments...]  e.g. *RunBox wperun /usr/bin/WPEWebDriver --version
 *
 * -d: detached -- the program runs on in a session of its own, its input
 * and output /dev/null, and wperun returns at once, so RISC OS carries on
 * (ROSGD's compositor, started from the desktop).
 *
 * The first time, it finds the disk -- the ext4 file system labelled
 * "boxwpe", by its superblock -- mounts it read-only at /wpe and gives it
 * what a Linux system expects: /dev (with /dev/pts), /proc and /sys, the
 * share at /host when there is one, and tmpfs at /tmp, /run, /var/tmp,
 * /root and /dev/shm.  /etc/resolv.conf in the root is a link to
 * /run/resolv.conf, which is the box's own (the kernel's DHCP), copied
 * there.  Then, each time, it changes root to /wpe and executes the
 * program there, with HOME, PATH, LANG and XDG_RUNTIME_DIR set, and
 * WebKit's sandbox off (it is bubblewrap, which the box cannot run).
 *
 * A static program for the box's Linux (LP64, musl), as *RunBox runs: it
 * is built on the Mac by ports/wpe/build.sh.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT "/wpe"
#define LABEL "boxwpe"

static int fail(const char *what)
{
    fprintf(stderr, "wperun: %s: %s\n", what, strerror(errno));
    return 127;
}

/* the block device whose ext4 superblock carries the label */
static int find_disk(char *dev, size_t size)
{
    DIR *d = opendir("/sys/class/block");
    int found = 0;
    for (struct dirent *e; d && !found && (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned char sb[256];
        if (pread(fd, sb, sizeof sb, 1024) == (ssize_t)sizeof sb && sb[56] == 0x53 && sb[57] == 0xEF &&
            strncmp((const char *)sb + 120, LABEL, 16) == 0) {
            snprintf(dev, size, "%s", path);
            found = 1;
        }
        close(fd);
    }
    if (d)
        closedir(d);
    return found;
}

static void tmpfs(const char *at, const char *mode)
{
    char opt[32];
    snprintf(opt, sizeof opt, "mode=%s", mode);
    mkdir(at, 0755);
    if (mount("tmpfs", at, "tmpfs", MS_NOSUID | MS_NODEV, opt) != 0)
        fprintf(stderr, "wperun: tmpfs at %s: %s\n", at, strerror(errno));
}

static void bind(const char *from, const char *to)
{
    struct stat st;
    if (stat(from, &st) != 0)
        return;
    if (mount(from, to, NULL, MS_BIND | MS_REC, NULL) != 0)
        fprintf(stderr, "wperun: %s on %s: %s\n", from, to, strerror(errno));
}

static void copy(const char *from, const char *to)
{
    char buf[4096];
    int in = open(from, O_RDONLY), out;
    if (in < 0)
        return;
    ssize_t n = read(in, buf, sizeof buf);
    close(in);
    if (n <= 0 || (out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        return;
    if (write(out, buf, (size_t)n) != n)
        fprintf(stderr, "wperun: %s: short write\n", to);
    close(out);
}

/* whether the kernel's command line has a word */
static int cmdline_has(const char *word)
{
    char buf[4096];
    int fd = open("/proc/cmdline", O_RDONLY), found = 0;
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    size_t len = strlen(word);
    for (char *p = buf; (p = strstr(p, word)) != NULL; p += len)
        if ((p == buf || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\n' || p[len] == 0))
            found = 1;
    return found;
}

static int set_up(void)
{
    struct stat st;
    if (stat(ROOT "/etc/boxwpe", &st) == 0) {
        /* mounted already -- perhaps by the compositor, at start-up, before
         * the network had its name servers: they are copied again */
        copy("/etc/resolv.conf", ROOT "/run/resolv.conf");
        return 0;
    }
    char dev[300];
    if (!find_disk(dev, sizeof dev)) {
        fprintf(stderr, "wperun: no disk labelled \"" LABEL "\" -- the WPE edition's root (ROSGD_WPE)\n");
        return 127;
    }
    mkdir(ROOT, 0755);
    if (mount(dev, ROOT, "ext4", MS_RDONLY | MS_NOATIME, "") != 0)
        return fail(dev);
    bind("/dev", ROOT "/dev");
    if (mount("proc", ROOT "/proc", "proc", 0, NULL) != 0)
        return fail(ROOT "/proc");
    if (mount("sysfs", ROOT "/sys", "sysfs", 0, NULL) != 0)
        return fail(ROOT "/sys");
    tmpfs(ROOT "/dev/shm", "1777");
    /* no GPU for WPE yet: RISC OS owns the display device, and WPE, finding
     * it, asks it for buffers and is refused.  An empty /dev/dri leaves
     * WPE shared memory and Mesa's software renderer (design 23, D3).
     * But with rosgd.display=compositor RISC OS does not use it, and the
     * compositor, which owns the display then, must see it. */
    if (!cmdline_has("rosgd.display=compositor"))
        tmpfs(ROOT "/dev/dri", "0755");
    tmpfs(ROOT "/tmp", "1777");
    tmpfs(ROOT "/var/tmp", "1777");
    tmpfs(ROOT "/run", "0755");
    tmpfs(ROOT "/root", "0700");
    bind("/host", ROOT "/host");
    copy("/etc/resolv.conf", ROOT "/run/resolv.conf");
    mkdir(ROOT "/run/user", 0700);
    return 0;
}

int main(int argc, char **argv)
{
    int detach = argc > 1 && strcmp(argv[1], "-d") == 0;
    if (detach)
        argv++, argc--;
    if (argc < 2) {
        fprintf(stderr, "usage: wperun [-d] <program> [arguments...]\n");
        return 2;
    }
    int r = set_up();
    if (r)
        return r;
    if (detach) {
        /* twice: the caller returns only once the program is in a session
         * of its own, so that *RunBox's pty closing (a hang-up to its
         * process group) cannot reach it */
        pid_t pid = fork();
        if (pid < 0)
            return fail("fork");
        if (pid > 0) {
            int st;
            waitpid(pid, &st, 0);
            return 0;                           /* the caller goes on */
        }
        setsid();
        signal(SIGHUP, SIG_IGN);
        pid = fork();
        if (pid < 0)
            _exit(1);
        if (pid > 0)
            _exit(0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0), dup2(null, 1), dup2(null, 2);
            if (null > 2)
                close(null);
        }
    }
    if (chroot(ROOT) != 0 || chdir("/") != 0)
        return fail("chroot " ROOT);
    /* WebKit's sandbox is bubblewrap, in the box kernel's namespaces
     * (design 23, R10).  A kernel without them (one from before 7 October
     * 2026) would leave WebKit unable to start a web process at all, so
     * there, and only there, the sandbox is turned off, and said so. */
    int sandbox = access("/proc/self/ns/user", F_OK) == 0;
    if (!sandbox)
        fprintf(stderr, "wperun: this kernel has no user namespaces: WebKit runs unsandboxed\n");
    char *env[] = {
        "HOME=/root",
        "PATH=/usr/local/bin:/usr/bin:/usr/sbin",
        "LANG=C.UTF-8",
        "XDG_RUNTIME_DIR=/run/user",
        "TERM=vt100",
        "LIBGL_ALWAYS_SOFTWARE=1",
        sandbox ? NULL : "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1",
        NULL,
    };
    execve(argv[1], argv + 1, env);
    return fail(argv[1]);
}
