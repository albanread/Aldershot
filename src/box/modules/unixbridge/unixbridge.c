/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* unixbridge.c -- the Unix bridge: Linux's system calls for RISC OS
 * programs, as SWI Unix_Syscall (include/rosgd/unixbridge.h).
 *
 * A POSIX application (rosgd/posix) is x32 code in the arena, a RISC OS
 * task, whose C library (musl) makes its system calls here. Each call is
 * handled one of four ways:
 *
 *   pass     to Linux: pointers are arena addresses, which are host
 *            addresses in the box; descriptors through the task's table;
 *            paths RISC OS's or Unix's, relative ones from the task's
 *            current directory;
 *   convert  the x32-only calls whose structures differ (iovec, ...);
 *   virtual  what is a process's and so must be the task's here: its
 *            descriptor table, current directory, umask, memory, signals,
 *            exit;
 *   refuse   ENOSYS, reported once per number on the serial console, so a
 *            gap shows itself.
 *
 * Each task is a process (UNIX_ROSGD_INIT makes it one). Descriptors 0-2 are
 * on the console (the VDU and the keyboard). Its current directory is the
 * CSD's Linux directory. It has a heap, which is a dynamic area of its own
 * holding its stack at the bottom, then brk's reach, then what mmap gives.
 * exit ends the task's program (OS_Exit), not /init. The process's state
 * goes with it.
 *
 * Blocking calls (reads, polls, sleeps) release the personality lock
 * (ROS_BLOCKING), as the runtime's own modules do. The Wimp baton stays with
 * the task, so a program that waits holds the desktop, as on RISC OS.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__)
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/unixbridge.h"
#include "ubsys.h"

#define PAGE          4096u
#define MAX_TASKS     64
#define MAX_FDS       256
#define MAX_RANGES    1024
#define HEAP_MAX      (256u << 20)      /* a task's dynamic area, most it may grow to */
#define BRK_RESERVE   (16u << 20)       /* brk's reach, above the stack */

#if defined(__linux__)

/* ---- a task's process state ------------------------------------------------ */

enum { FD_FREE, FD_HOST, FD_CONSOLE_IN, FD_CONSOLE_OUT };

struct ub_fd {
    int kind;
    int host;                   /* FD_HOST: its Linux descriptor */
    int cloexec;
};

struct ub_range {
    uint32_t lo, len;
};

struct ub_task {
    int used;
    uint32_t id;                /* ros_task_id */
    struct ros_task *task;
    uint32_t pid;
    struct ub_fd fd[MAX_FDS];
    int cwd;                    /* a Linux descriptor of its current directory */
    char cwd_path[PATH_MAX];
    unsigned umask;
    /* memory: the heap's dynamic area */
    uint32_t da, da_base, da_max, da_size;
    uint32_t da_name;           /* its name, in the RMA (the area keeps the pointer) */
    uint32_t brk_lo, brk_cur, brk_hi;
    struct ub_range free[MAX_RANGES];   /* mmap's free ranges, by address */
    unsigned nfree;
    uint32_t high;              /* the highest address mmap or brk has given */
    /* signals, recorded but not yet delivered */
    uint32_t sa[65][5];
    uint64_t sigmask;
    struct termios tios;
};

static struct ub_task tasks[MAX_TASKS];
static uint32_t next_pid = UNIX_PID_BASE;

/* The calling task's process, or NULL if it has not started one */
static struct ub_task *current(void)
{
    struct ros_task *t = ros_task_current();
    uint32_t id = t ? ros_task_id(t) : 0;
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used && tasks[i].id == id && tasks[i].task == t)
            return &tasks[i];
    return NULL;
}

/* ---- RISC OS calls ------------------------------------------------------------ */

static os_error *swi(uint32_t number, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

/* ---- the console: descriptors 0-2 --------------------------------------------- */

static void console_defaults(struct termios *t)
{
    memset(t, 0, sizeof *t);
    t->c_iflag = ICRNL;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    t->c_lflag = ICANON | ECHO | ISIG | IEXTEN | ECHOE | ECHOK;
    t->c_cc[VINTR] = 3, t->c_cc[VQUIT] = 28, t->c_cc[VERASE] = 127, t->c_cc[VKILL] = 21;
    t->c_cc[VEOF] = 4, t->c_cc[VMIN] = 1;
}

/* Unix's newline is RISC OS's OS_NewLine (LF, CR) on the VDU */
static int64_t console_write(uint32_t buf, uint64_t len)
{
    const uint8_t *p = ros_ptr(buf);
    uint64_t i = 0;
    while (i < len) {
        uint64_t j = i;
        while (j < len && p[j] != '\n')
            j++;
        uint32_t r[10] = { buf + (uint32_t)i, (uint32_t)(j - i) };
        if (j > i && swi(OS_WriteN, r))
            return -EIO;
        if (j < len) {
            uint32_t n[10] = { 0 };
            if (swi(OS_NewLine, n))
                return -EIO;
            j++;
        }
        i = j;
    }
    return (int64_t)len;
}

/* A line, edited as OS_ReadLine edits one, with its newline. Returns -EINTR at Escape */
static int64_t console_read(uint32_t buf, uint64_t len)
{
    if (len < 2)
        return len ? -EINVAL : 0;
    uint32_t r[10] = { buf, (uint32_t)(len - 1 > 255 ? 255 : len - 1), 32, 255, 0 };
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, sizeof r);
    ros_swi(&s, OS_ReadLine32 | ROS_X_BIT);
    if (s.v)
        return -EIO;
    if (s.c)
        return -EINTR;                  /* Escape */
    uint8_t *p = ros_ptr(buf);
    p[s.r[1]] = '\n';
    return (int64_t)s.r[1] + 1;
}

/* ---- descriptors ------------------------------------------------------------------ */

static int fd_alloc(struct ub_task *t, int from)
{
    for (int i = from < 0 ? 0 : from; i < MAX_FDS; i++)
        if (t->fd[i].kind == FD_FREE)
            return i;
    return -EMFILE;
}

static struct ub_fd *fd_get(struct ub_task *t, int64_t fd)
{
    if (fd < 0 || fd >= MAX_FDS || t->fd[fd].kind == FD_FREE)
        return NULL;
    return &t->fd[fd];
}

/* The Linux descriptor behind fd, or -EBADF (the console has none) */
static int host_fd(struct ub_task *t, int64_t fd)
{
    struct ub_fd *f = fd_get(t, fd);
    if (!f)
        return -EBADF;
    return f->kind == FD_HOST ? f->host : -ENOTSUP;
}

static void fd_close(struct ub_fd *f)
{
    if (f->kind == FD_HOST)
        close(f->host);
    memset(f, 0, sizeof *f);
}

/* A new descriptor for a Linux one the task now owns */
static int64_t fd_install(struct ub_task *t, int host, int from, int cloexec)
{
    int fd = fd_alloc(t, from);
    if (fd < 0) {
        close(host);
        return fd;
    }
    t->fd[fd] = (struct ub_fd){ FD_HOST, host, cloexec };
    return fd;
}

/* ---- paths: RISC OS's or Unix's ----------------------------------------------------- */

/* A RISC OS name has a filing system or path variable ("HostFS::Disc.$.a",
 * "Choices:x"), a variable in angle brackets, or starts at one of RISC OS's
 * special directories.  Anything else is a Unix path. */
static int riscos_name(const char *s)
{
    if (strchr(s, '<'))
        return 1;
    if (*s == '$' || *s == '@' || *s == '^' || *s == '&' || *s == '%' || *s == '\\')
        return s[1] == 0 || s[1] == '.';
    const char *colon = strchr(s, ':'), *slash = strchr(s, '/');
    return colon && colon > s && (!slash || colon < slash);
}

/* The path at arena address a, for Linux: *dirfd the directory it is
 * relative to (the task's current one) or AT_FDCWD for an absolute path */
static int64_t path_arg(struct ub_task *t, uint32_t a, char *out, size_t max, int *dirfd)
{
    if (!a)
        return -EFAULT;
    const char *s = ros_ptr(a);
    size_t n = strnlen(s, PATH_MAX);
    if (n >= PATH_MAX)
        return -ENAMETOOLONG;
    *dirfd = AT_FDCWD;
    if (riscos_name(s)) {
        if (ros_hostfs_linux_path(s, out, max))
            return -ENOENT;
        return 0;
    }
    if (n >= max)
        return -ENAMETOOLONG;
    memcpy(out, s, n + 1);
    if (*out != '/')
        *dirfd = t->cwd;
    return 0;
}

/* An *at call's directory: AT_FDCWD is the task's own */
static int dir_arg(struct ub_task *t, int64_t dirfd, int given)
{
    if (given == AT_FDCWD && dirfd == AT_FDCWD)
        return t->cwd;
    if ((int)dirfd == AT_FDCWD)
        return given;
    return host_fd(t, dirfd);
}

/* ---- memory: the heap's dynamic area ------------------------------------------------ */

static void grow_area(struct ub_task *t, uint32_t top)
{
    if (top <= t->high)
        return;
    t->high = top;
    uint32_t want = top - t->da_base;
    if (want > t->da_size) {
        uint32_t r[10] = { t->da, want - t->da_size };
        if (!swi(OS_ChangeDynamicArea, r))
            t->da_size += r[1];
    }
}

static void range_free(struct ub_task *t, uint32_t lo, uint32_t len)
{
    unsigned i = 0;
    while (i < t->nfree && t->free[i].lo < lo)
        i++;
    /* join the one before, the one after, or both */
    if (i > 0 && t->free[i - 1].lo + t->free[i - 1].len == lo) {
        t->free[i - 1].len += len;
        if (i < t->nfree && lo + len == t->free[i].lo) {
            t->free[i - 1].len += t->free[i].len;
            memmove(&t->free[i], &t->free[i + 1], (t->nfree - i - 1) * sizeof t->free[0]);
            t->nfree--;
        }
        return;
    }
    if (i < t->nfree && lo + len == t->free[i].lo) {
        t->free[i].lo = lo;
        t->free[i].len += len;
        return;
    }
    if (t->nfree == MAX_RANGES)
        return;                         /* too fragmented: the range is lost */
    memmove(&t->free[i + 1], &t->free[i], (t->nfree - i) * sizeof t->free[0]);
    t->free[i] = (struct ub_range){ lo, len };
    t->nfree++;
}

/* len bytes (a page multiple), zeroed, or 0 */
static uint32_t range_alloc(struct ub_task *t, uint32_t len)
{
    for (unsigned i = 0; i < t->nfree; i++) {
        if (t->free[i].len < len)
            continue;
        uint32_t lo = t->free[i].lo;
        t->free[i].lo += len;
        t->free[i].len -= len;
        if (!t->free[i].len) {
            memmove(&t->free[i], &t->free[i + 1], (t->nfree - i - 1) * sizeof t->free[0]);
            t->nfree--;
        }
        grow_area(t, lo + len);
        memset(ros_ptr(lo), 0, len);
        return lo;
    }
    return 0;
}

static int in_heap(struct ub_task *t, uint64_t a, uint64_t len)
{
    return a >= t->brk_hi && a + len <= (uint64_t)t->da_base + t->da_max && a + len >= a;
}

static int64_t sys_mmap(struct ub_task *t, const int64_t *a)
{
    uint64_t addr = (uint64_t)a[0], len = (uint64_t)a[1];
    int prot = (int)a[2], flags = (int)a[3], fd = (int)a[4];
    uint64_t off = (uint64_t)a[5];
    if (!len || len > t->da_max)
        return -ENOMEM;
    uint32_t n = (uint32_t)((len + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    /* A file mapped shared and writable would have to write back, which is not
     * done yet. Read-only, a shared mapping shows what a private one does
     * unless the file changes under it, so it is a copy too (NetSurf's file
     * fetcher). */
    if (flags & MAP_SHARED && !(flags & MAP_ANONYMOUS) && prot & PROT_WRITE)
        return -ENODEV;
    uint32_t lo;
    if (flags & MAP_FIXED) {
        if (addr & (PAGE - 1) || !in_heap(t, addr, n))
            return -EINVAL;
        lo = (uint32_t)addr;            /* already the program's: it is reusing it */
        memset(ros_ptr(lo), 0, n);
        grow_area(t, lo + n);
    } else if (!(lo = range_alloc(t, n)))
        return -ENOMEM;
    if (!(flags & MAP_ANONYMOUS)) {
        /* a private copy of the file: what MAP_PRIVATE shows until written */
        int h = host_fd(t, fd);
        if (h < 0) {
            range_free(t, lo, n);
            return h;
        }
        uint64_t done = 0;
        while (done < len) {
            ssize_t got;
            ROS_BLOCKING(got = pread(h, (char *)ros_ptr(lo) + done, (size_t)(len - done),
                                     (off_t)(off + done)));
            if (got < 0) {
                range_free(t, lo, n);
                return -errno;
            }
            if (!got)
                break;                  /* past the file's end: the rest stays zero */
            done += (uint64_t)got;
        }
    }
    return lo;
}

static int64_t sys_munmap(struct ub_task *t, uint64_t addr, uint64_t len)
{
    uint32_t n = (uint32_t)((len + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    if (addr & (PAGE - 1) || !n || !in_heap(t, addr, n))
        return -EINVAL;
    range_free(t, (uint32_t)addr, n);
    return 0;
}

static int64_t sys_brk(struct ub_task *t, uint64_t want)
{
    if (want >= t->brk_lo && want <= t->brk_hi) {
        if (want > t->brk_cur) {
            memset(ros_ptr(t->brk_cur), 0, (size_t)(want - t->brk_cur));
            grow_area(t, (uint32_t)want);
        }
        t->brk_cur = (uint32_t)want;
    }
    return t->brk_cur;
}

/* ---- the process: made, and ended ---------------------------------------------------- */

static void release(struct ub_task *t)
{
    for (int i = 0; i < MAX_FDS; i++)
        fd_close(&t->fd[i]);
    if (t->cwd >= 0)
        close(t->cwd);
    if (t->da) {
        uint32_t r[10] = { 1, t->da };
        swi(OS_DynamicArea, r);
    }
    if (t->da_name)
        ros_rma_free(ros_ptr(t->da_name));
    memset(t, 0, sizeof *t);
}

static void set_cwd(struct ub_task *t, int fd, const char *path)
{
    if (t->cwd >= 0)
        close(t->cwd);
    t->cwd = fd;
    snprintf(t->cwd_path, sizeof t->cwd_path, "%s", path);
}

static int64_t sys_init(uint32_t a)
{
    struct unix_init *in = ros_ptr(a);
    if (!a || in->version != UNIX_INIT_VERSION)
        return -EINVAL;
    struct ros_task *rt = ros_task_current();
    uint32_t id = rt ? ros_task_id(rt) : 0;

    /* A process that the task's last program left (it ended without exit), and
     * those of tasks that have ended, go first. */
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used && ((tasks[i].id == id && tasks[i].task == rt) ||
                              (tasks[i].task && ros_task_ended(tasks[i].task))))
            release(&tasks[i]);
    struct ub_task *t = NULL;
    for (unsigned i = 0; i < MAX_TASKS && !t; i++)
        if (!tasks[i].used)
            t = &tasks[i];
    if (!t)
        return -EAGAIN;
    memset(t, 0, sizeof *t);
    t->used = 1, t->id = id, t->task = rt, t->pid = next_pid++;
    t->cwd = -1;
    t->umask = 022;
    console_defaults(&t->tios);
    t->fd[0] = (struct ub_fd){ FD_CONSOLE_IN, -1, 0 };
    t->fd[1] = (struct ub_fd){ FD_CONSOLE_OUT, -1, 0 };
    t->fd[2] = (struct ub_fd){ FD_CONSOLE_OUT, -1, 0 };

    /* The current directory: the CSD, where it is a Linux directory */
    char csd[PATH_MAX];
    int dfd = -1;
    if (!ros_hostfs_linux_path("@", csd, sizeof csd))
        dfd = open(csd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        snprintf(csd, sizeof csd, "/");
        dfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    set_cwd(t, dfd, csd);

    /* The heap: a dynamic area named after the program */
    const char *prog = in->name ? (const char *)ros_ptr(in->name) : "", *start = prog;
    for (const char *p = prog; *p > ' '; p++)
        if (*p == '.' || *p == ':' || *p == '/')
            start = p + 1;
    char leaf[48];
    size_t k = 0;
    while (start[k] > ' ' && k < sizeof leaf - 1)
        leaf[k] = start[k], k++;
    leaf[k] = 0;
    char *name = ros_rma_alloc(64);
    if (!name) {
        release(t);
        return -ENOMEM;
    }
    if (in->heap_name)
        snprintf(name, 64, "%s", (const char *)ros_ptr(in->heap_name));
    else
        snprintf(name, 64, "%s heap", leaf[0] ? leaf : "POSIX");
    t->da_name = ros_addr(name);
    uint32_t stack = (in->stack_size + PAGE - 1) & ~(PAGE - 1);
    if (!stack || stack > (64u << 20))
        stack = 1u << 20;
    uint32_t max = in->heap_size >= (16u << 20) && in->heap_size <= (1u << 30)
                       ? (in->heap_size + PAGE - 1) & ~(PAGE - 1) : HEAP_MAX;
    uint32_t r[10] = { 0, 0xFFFFFFFFu, PAGE + stack, 0xFFFFFFFFu, 0x80, max, 0, 0,
                       t->da_name };
    if (swi(OS_DynamicArea, r)) {
        release(t);
        return -ENOMEM;
    }
    t->da = r[1], t->da_base = r[3], t->da_max = r[5], t->da_size = PAGE + stack;
    t->high = t->da_base + t->da_size;

    /* The stack at the bottom, over a guard page. Then brk's reach. Then mmap's */
    mprotect(ros_ptr(t->da_base), PAGE, PROT_NONE);
    uint32_t stack_top = t->da_base + PAGE + stack;
    t->brk_lo = t->brk_cur = stack_top;
    t->brk_hi = t->brk_lo + BRK_RESERVE;
    t->nfree = 0;
    range_free(t, t->brk_hi, t->da_base + t->da_max - t->brk_hi);

    in->stack_top = stack_top & ~15u;
    in->heap_base = t->da_base;
    in->heap_max = t->da_max;
    in->pid = t->pid;
    return 0;
}

/* exit: the process goes, and the task's program ends with the status as
 * its return code (OS_Exit "ABEX", Sys$ReturnCode) */
static void sys_exit(struct ub_task *t, int status)
{
    release(t);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0;
    s.r[1] = 0x58454241u;                     /* "ABEX" */
    s.r[2] = (uint32_t)(status & 0xFF);
    ros_swi(&s, OS_Exit);
}

/* ---- the calls ------------------------------------------------------------------------- */

struct iovec32 {
    uint32_t base, len;
};

/* An iovec array for Linux: x32's converted, x86-64's as it is */
static int64_t iov_arg(uint32_t a, uint64_t n, int x32, struct iovec *out)
{
    if (n > IOV_MAX)
        return -EINVAL;
    if (x32) {
        const struct iovec32 *v = ros_ptr(a);
        for (uint64_t i = 0; i < n; i++)
            out[i] = (struct iovec){ ros_ptr(v[i].base), v[i].len };
    } else {
        memcpy(out, ros_ptr(a), n * sizeof *out);
    }
    return 0;
}

static int64_t rw_console(struct ub_fd *f, int wr, uint32_t buf, uint64_t len)
{
    if (wr)
        return f->kind == FD_CONSOLE_OUT ? console_write(buf, len) : -EBADF;
    return f->kind == FD_CONSOLE_IN ? console_read(buf, len) : -EBADF;
}

static int64_t sys_rw(struct ub_task *t, int wr, int64_t fd, uint32_t buf, uint64_t len)
{
    struct ub_fd *f = fd_get(t, fd);
    if (!f)
        return -EBADF;
    if (f->kind != FD_HOST)
        return rw_console(f, wr, buf, len);
    ssize_t r;
    if (wr)
        ROS_BLOCKING(r = write(f->host, ros_ptr(buf), (size_t)len));
    else
        ROS_BLOCKING(r = read(f->host, ros_ptr(buf), (size_t)len));
    return r < 0 ? -errno : r;
}

static int64_t sys_rwv(struct ub_task *t, int wr, int64_t fd, uint32_t iov, uint64_t n,
                       int x32)
{
    struct iovec v[IOV_MAX];
    int64_t e = iov_arg(iov, n, x32, v);
    if (e)
        return e;
    struct ub_fd *f = fd_get(t, fd);
    if (!f)
        return -EBADF;
    if (f->kind != FD_HOST) {
        int64_t total = 0;
        for (uint64_t i = 0; i < n; i++) {
            if (!v[i].iov_len)
                continue;
            int64_t r = rw_console(f, wr, ros_addr(v[i].iov_base), v[i].iov_len);
            if (r < 0)
                return total ? total : r;
            total += r;
            if ((uint64_t)r < v[i].iov_len)
                break;
        }
        return total;
    }
    ssize_t r;
    if (wr)
        ROS_BLOCKING(r = writev(f->host, v, (int)n));
    else
        ROS_BLOCKING(r = readv(f->host, v, (int)n));
    return r < 0 ? -errno : r;
}

static int64_t sys_ioctl(struct ub_task *t, int64_t fd, uint64_t req, uint32_t arg)
{
    struct ub_fd *f = fd_get(t, fd);
    if (!f)
        return -EBADF;
    if (f->kind != FD_HOST) {
        switch (req) {
        case TIOCGWINSZ: {
            struct winsize *w = ros_ptr(arg);
            *w = (struct winsize){ 32, 80, 0, 0 };
            return 0;
        }
        case TCGETS:
            memcpy(ros_ptr(arg), &t->tios, sizeof t->tios);
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            memcpy(&t->tios, ros_ptr(arg), sizeof t->tios);
            return 0;
        default:
            return -ENOTTY;
        }
    }
    int r = ioctl(f->host, (unsigned long)req, ros_ptr(arg));
    return r < 0 ? -errno : r;
}

static int64_t sys_fcntl(struct ub_task *t, int64_t fd, int cmd, int64_t arg)
{
    struct ub_fd *f = fd_get(t, fd);
    if (!f)
        return -EBADF;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (f->kind != FD_HOST) {
            int n = fd_alloc(t, (int)arg);
            if (n < 0)
                return n;
            t->fd[n] = (struct ub_fd){ f->kind, -1, cmd == F_DUPFD_CLOEXEC };
            return n;
        }
        int h = fcntl(f->host, F_DUPFD_CLOEXEC, 3);
        return h < 0 ? -errno : fd_install(t, h, (int)arg, cmd == F_DUPFD_CLOEXEC);
    }
    case F_GETFD:
        return f->cloexec ? FD_CLOEXEC : 0;
    case F_SETFD:
        f->cloexec = !!(arg & FD_CLOEXEC);
        return 0;
    default:
        if (f->kind != FD_HOST)
            return cmd == F_GETFL ? (f->kind == FD_CONSOLE_IN ? O_RDONLY : O_WRONLY) : 0;
        int r = fcntl(f->host, cmd, (long)arg);
        return r < 0 ? -errno : r;
    }
}

static int64_t sys_dup3(struct ub_task *t, int64_t old, int64_t new, int flags, int any)
{
    struct ub_fd *f = fd_get(t, old);
    if (!f)
        return -EBADF;
    if (!any && (new < 0 || new >= MAX_FDS))
        return -EBADF;
    if (!any && old == new)
        return new;
    int n = any ? fd_alloc(t, 0) : (int)new;
    if (n < 0)
        return n;
    struct ub_fd copy = *f;
    if (f->kind == FD_HOST && (copy.host = fcntl(f->host, F_DUPFD_CLOEXEC, 3)) < 0)
        return -errno;
    fd_close(&t->fd[n]);
    copy.cloexec = !!(flags & O_CLOEXEC);
    t->fd[n] = copy;
    return n;
}

static int64_t sys_pipe(struct ub_task *t, uint32_t a, int flags)
{
    int h[2];
    if (pipe2(h, O_CLOEXEC | (flags & O_NONBLOCK)) < 0)
        return -errno;
    int64_t r0 = fd_install(t, h[0], 0, !!(flags & O_CLOEXEC));
    if (r0 < 0) {
        close(h[1]);
        return r0;
    }
    int64_t r1 = fd_install(t, h[1], 0, !!(flags & O_CLOEXEC));
    if (r1 < 0) {
        fd_close(&t->fd[r0]);
        return r1;
    }
    int32_t *out = ros_ptr(a);
    out[0] = (int32_t)r0, out[1] = (int32_t)r1;
    return 0;
}

static int64_t sys_poll(struct ub_task *t, uint32_t a, uint64_t n, int timeout_ms)
{
    struct pollfd *p = ros_ptr(a);
    if (n > MAX_FDS)
        return -EINVAL;
    struct pollfd h[MAX_FDS];
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        struct ub_fd *f = p[i].fd < 0 ? NULL : fd_get(t, p[i].fd);
        h[i] = (struct pollfd){ -1, p[i].events, 0 };
        p[i].revents = 0;
        if (p[i].fd < 0)
            continue;
        if (!f)
            p[i].revents = POLLNVAL, ready++;
        else if (f->kind == FD_HOST)
            h[i].fd = f->host;
        else if (f->kind == FD_CONSOLE_OUT && p[i].events & POLLOUT)
            p[i].revents = POLLOUT, ready++;
    }
    int r;
    ROS_BLOCKING(r = poll(h, (nfds_t)n, ready ? 0 : timeout_ms));
    if (r < 0)
        return -errno;
    for (uint64_t i = 0; i < n; i++)
        if (h[i].fd >= 0 && h[i].revents)
            p[i].revents = h[i].revents, ready++;
    return ready;
}

/* ---- sockets ------------------------------------------------------------------------ */

/* x32's struct msghdr: every pointer and length a 32-bit word (28 bytes) */
struct msghdr32 {
    uint32_t name, namelen, iov, iovlen, control, controllen;
    int32_t flags;
};

/* sendmsg and recvmsg. x32's header is converted, and x86-64's is used as it
 * is. Control messages (descriptor passing and the like) are not carried
 * yet, and a header with any is refused. */
static int64_t sys_msg(struct ub_task *t, int send, int64_t fd, uint32_t a, int flags, int x32)
{
    int h = host_fd(t, fd);
    if (h < 0)
        return h == -ENOTSUP ? -ENOTSOCK : h;
    struct msghdr m;
    struct iovec v[IOV_MAX];
    struct msghdr32 *m32 = NULL;
    if (x32) {
        m32 = ros_ptr(a);
        if (m32->controllen)
            return -EOPNOTSUPP;
        int64_t e = iov_arg(m32->iov, m32->iovlen, 1, v);
        if (e)
            return e;
        m = (struct msghdr){ .msg_name = m32->name ? ros_ptr(m32->name) : NULL,
                             .msg_namelen = m32->namelen, .msg_iov = v,
                             .msg_iovlen = (int)m32->iovlen, .msg_flags = m32->flags };
    } else {
        m = *(struct msghdr *)ros_ptr(a);
        if (m.msg_controllen)
            return -EOPNOTSUPP;
    }
    ssize_t r;
    if (send)
        ROS_BLOCKING(r = sendmsg(h, &m, flags | MSG_NOSIGNAL));
    else
        ROS_BLOCKING(r = recvmsg(h, &m, flags));
    if (r < 0)
        return -errno;
    if (!send) {
        if (m32)
            m32->namelen = m.msg_namelen, m32->flags = m.msg_flags;
        else
            ((struct msghdr *)ros_ptr(a))->msg_namelen = m.msg_namelen,
            ((struct msghdr *)ros_ptr(a))->msg_flags = m.msg_flags;
    }
    return r;
}

/* select and pselect6. The program's sets are of its own descriptors, and
 * Linux's are of Linux's. Each set is translated there and back. The console
 * is always writable and never readable here (a line is read with read). */
static int64_t sys_select(struct ub_task *t, int64_t nfds, uint32_t ra, uint32_t wa, uint32_t ea,
                          const struct timespec *ts)
{
    if (nfds < 0 || nfds > FD_SETSIZE)
        return -EINVAL;
    fd_set *pr = ra ? ros_ptr(ra) : NULL, *pw = wa ? ros_ptr(wa) : NULL,
           *pe = ea ? ros_ptr(ea) : NULL;
    fd_set hr, hw, he;
    FD_ZERO(&hr);
    FD_ZERO(&hw);
    FD_ZERO(&he);
    int hmax = -1, ready = 0;
    int map[FD_SETSIZE];
    for (int fd = 0; fd < nfds; fd++) {
        int want = (pr && FD_ISSET(fd, pr)) || (pw && FD_ISSET(fd, pw)) || (pe && FD_ISSET(fd, pe));
        map[fd] = -1;
        if (!want)
            continue;
        struct ub_fd *f = fd_get(t, fd);
        if (!f)
            return -EBADF;
        if (f->kind != FD_HOST)
            continue;
        map[fd] = f->host;
        if (f->host >= FD_SETSIZE)
            return -EINVAL;
        if (pr && FD_ISSET(fd, pr)) FD_SET(f->host, &hr);
        if (pw && FD_ISSET(fd, pw)) FD_SET(f->host, &hw);
        if (pe && FD_ISSET(fd, pe)) FD_SET(f->host, &he);
        if (f->host > hmax) hmax = f->host;
    }
    int console_out = 0;
    for (int fd = 0; fd < nfds; fd++)
        if (pw && FD_ISSET(fd, pw) && map[fd] < 0 && fd_get(t, fd)->kind == FD_CONSOLE_OUT)
            console_out = 1;
    struct timespec zero = { 0, 0 };
    int r;
    ROS_BLOCKING(r = pselect(hmax + 1, &hr, &hw, &he, console_out ? &zero : ts, NULL));
    if (r < 0)
        return -errno;
    for (int fd = 0; fd < nfds; fd++) {
        int h = map[fd];
        int console = h < 0 && ((pr && FD_ISSET(fd, pr)) || (pw && FD_ISSET(fd, pw)) ||
                                (pe && FD_ISSET(fd, pe)));
        if (pr && FD_ISSET(fd, pr) && !(h >= 0 && FD_ISSET(h, &hr)))
            FD_CLR(fd, pr);
        if (pw && FD_ISSET(fd, pw)) {
            if (console && fd_get(t, fd)->kind == FD_CONSOLE_OUT)
                ready++;
            else if (!(h >= 0 && FD_ISSET(h, &hw)))
                FD_CLR(fd, pw);
            else
                ready++;
        }
        if (pe && FD_ISSET(fd, pe) && !(h >= 0 && FD_ISSET(h, &he)))
            FD_CLR(fd, pe);
        if (pr && FD_ISSET(fd, pr))
            ready++;
        if (pe && FD_ISSET(fd, pe))
            ready++;
    }
    return ready;
}

static int64_t sys_chdir(struct ub_task *t, int dirfd, const char *path)
{
    int fd = openat(dirfd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    char buf[PATH_MAX], link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, buf, sizeof buf - 1);
    buf[n < 0 ? 0 : n] = 0;
    set_cwd(t, fd, n < 0 ? path : buf);
    return 0;
}

static int64_t sys_getcwd(struct ub_task *t, uint32_t a, uint64_t size)
{
    size_t n = strlen(t->cwd_path) + 1;
    if (n > size)
        return -ERANGE;
    memcpy(ros_ptr(a), t->cwd_path, n);
    return (int64_t)n;
}

/* A signal to the process itself ends it, as the default action would, unless
 * it is ignored or is one that is ignored by default (SIGCHLD, SIGURG,
 * SIGWINCH). Handlers are not called yet */
static int64_t sys_kill_self(struct ub_task *t, int sig)
{
    if (sig == 0)
        return 0;
    if (sig < 1 || sig > 64)
        return -EINVAL;
    if (t->sa[sig][0] == 1 /* SIG_IGN */ || sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH)
        return 0;
    char msg[64];
    snprintf(msg, sizeof msg, "%s\r\n", sig == SIGABRT ? "Aborted" : "Killed by a signal");
    uint32_t r[10] = { 0 };
    char *m = ros_rma_alloc(sizeof msg);
    if (m) {
        memcpy(m, msg, sizeof msg);
        r[0] = ros_addr(m);
        swi(OS_Write0, r);
        ros_rma_free(m);
    }
    sys_exit(t, 128 + sig);
    return 0;
}

/* The first refusal of each call, on the serial console */
static void refuse_once(uint32_t nr)
{
    static uint8_t said[(UNIX_ROSGD_BASE + 64) / 8];
    if (nr < UNIX_ROSGD_BASE + 64 && !(said[nr / 8] & (1u << (nr % 8)))) {
        said[nr / 8] |= (uint8_t)(1u << (nr % 8));
        ros_console_printf("unixbridge: system call %u is not supported yet\n", nr);
    }
}

#define PATH(i, out)                                                           \
    char out[PATH_MAX];                                                        \
    int out##_dir;                                                             \
    do {                                                                       \
        int64_t pe_ = path_arg(t, (uint32_t)a[i], out, sizeof out, &out##_dir);  \
        if (pe_)                                                               \
            return pe_;                                                        \
    } while (0)
#define RET(expr) do { long r_ = (long)(expr); return r_ < 0 ? -errno : r_; } while (0)

static int64_t syscall_x(uint32_t nr, int x32, int64_t *a)
{
    if (nr == UNIX_ROSGD_INIT)
        return sys_init((uint32_t)a[0]);
    struct ub_task *t = current();
    if (!t)
        return -ENOSYS;                 /* not a process: UNIX_ROSGD_INIT first */
    (void)x32;                          /* x32's own layouts come by their own numbers */
    int h;

    switch (nr) {
    /* ---- descriptors: reading, writing */
    case UB_read:
        return sys_rw(t, 0, a[0], (uint32_t)a[1], (uint64_t)a[2]);
    case UB_write:
        return sys_rw(t, 1, a[0], (uint32_t)a[1], (uint64_t)a[2]);
    case UB_X32_readv:
    case UB_readv:
        return sys_rwv(t, 0, a[0], (uint32_t)a[1], (uint64_t)a[2], nr == UB_X32_readv);
    case UB_X32_writev:
    case UB_writev:
        return sys_rwv(t, 1, a[0], (uint32_t)a[1], (uint64_t)a[2], nr == UB_X32_writev);
    case UB_pread64:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ESPIPE : h;
        {
            ssize_t r;
            ROS_BLOCKING(r = pread(h, ros_ptr((uint32_t)a[1]), (size_t)a[2], (off_t)a[3]));
            return r < 0 ? -errno : r;
        }
    case UB_pwrite64:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ESPIPE : h;
        {
            ssize_t r;
            ROS_BLOCKING(r = pwrite(h, ros_ptr((uint32_t)a[1]), (size_t)a[2], (off_t)a[3]));
            return r < 0 ? -errno : r;
        }
    case UB_lseek:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ESPIPE : h;
        RET(lseek(h, (off_t)a[1], (int)a[2]));
    case UB_X32_ioctl:
    case UB_ioctl:
        return sys_ioctl(t, a[0], (uint64_t)a[1], (uint32_t)a[2]);
    case UB_fcntl:
        return sys_fcntl(t, a[0], (int)a[1], a[2]);
    case UB_close: {
        struct ub_fd *f = fd_get(t, a[0]);
        if (!f)
            return -EBADF;
        fd_close(f);
        return 0;
    }
    case UB_dup:
        return sys_dup3(t, a[0], 0, 0, 1);
    case UB_dup2:
        return sys_dup3(t, a[0], a[1], 0, 0);
    case UB_dup3:
        return a[0] == a[1] ? -EINVAL : sys_dup3(t, a[0], a[1], (int)a[2], 0);
    case UB_pipe:
        return sys_pipe(t, (uint32_t)a[0], 0);
    case UB_pipe2:
        return sys_pipe(t, (uint32_t)a[0], (int)a[1]);
    case UB_poll:
        return sys_poll(t, (uint32_t)a[0], (uint64_t)a[1], (int)a[2]);
    case UB_fstat:
        if ((h = host_fd(t, a[0])) >= 0)
            RET(fstat(h, ros_ptr((uint32_t)a[1])));
        if (h == -ENOTSUP) {                    /* the console: a character device */
            struct stat *st = ros_ptr((uint32_t)a[1]);
            memset(st, 0, sizeof *st);
            st->st_mode = S_IFCHR | 0620;
            st->st_blksize = 1024;
            return 0;
        }
        return h;
    case UB_fsync:
    case UB_fdatasync:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? 0 : h;
        RET(fsync(h));
    case UB_ftruncate:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -EINVAL : h;
        RET(ftruncate(h, (off_t)a[1]));
    case UB_getdents64:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTDIR : h;
        RET(syscall(SYS_getdents64, h, ros_ptr((uint32_t)a[1]), (unsigned)a[2]));
    case UB_fchdir:
        if ((h = host_fd(t, a[0])) < 0)
            return h;
        return sys_chdir(t, h, ".");

    /* ---- sockets */
    case UB_socket: {
        int fd = socket((int)a[0], (int)a[1] | SOCK_CLOEXEC, (int)a[2]);
        return fd < 0 ? -errno : fd_install(t, fd, 0, !!(a[1] & SOCK_CLOEXEC));
    }
    case UB_socketpair: {
        int hp[2];
        if (socketpair((int)a[0], (int)a[1] | SOCK_CLOEXEC, (int)a[2], hp) < 0)
            return -errno;
        int64_t r0 = fd_install(t, hp[0], 0, !!(a[1] & SOCK_CLOEXEC));
        if (r0 < 0) {
            close(hp[1]);
            return r0;
        }
        int64_t r1 = fd_install(t, hp[1], 0, !!(a[1] & SOCK_CLOEXEC));
        if (r1 < 0) {
            fd_close(&t->fd[r0]);
            return r1;
        }
        int32_t *out = ros_ptr((uint32_t)a[3]);
        out[0] = (int32_t)r0, out[1] = (int32_t)r1;
        return 0;
    }
    case UB_connect: {
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        int r;
        ROS_BLOCKING(r = connect(h, ros_ptr((uint32_t)a[1]), (socklen_t)a[2]));
        return r < 0 ? -errno : 0;
    }
    case UB_bind:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        RET(bind(h, ros_ptr((uint32_t)a[1]), (socklen_t)a[2]));
    case UB_listen:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        RET(listen(h, (int)a[1]));
    case UB_accept:
    case UB_accept4: {
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        int flags = nr == UB_accept4 ? (int)a[3] : 0, fd;
        ROS_BLOCKING(fd = accept4(h, a[1] ? ros_ptr((uint32_t)a[1]) : NULL,
                                  a[2] ? ros_ptr((uint32_t)a[2]) : NULL, flags | SOCK_CLOEXEC));
        return fd < 0 ? -errno : fd_install(t, fd, 0, !!(flags & SOCK_CLOEXEC));
    }
    case UB_sendto: {
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        ssize_t r;
        ROS_BLOCKING(r = sendto(h, ros_ptr((uint32_t)a[1]), (size_t)a[2], (int)a[3] | MSG_NOSIGNAL,
                                a[4] ? ros_ptr((uint32_t)a[4]) : NULL, (socklen_t)a[5]));
        return r < 0 ? -errno : r;
    }
    case UB_X32_recvfrom:
    case UB_recvfrom: {
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        ssize_t r;
        ROS_BLOCKING(r = recvfrom(h, ros_ptr((uint32_t)a[1]), (size_t)a[2], (int)a[3],
                                  a[4] ? ros_ptr((uint32_t)a[4]) : NULL,
                                  a[5] ? ros_ptr((uint32_t)a[5]) : NULL));
        return r < 0 ? -errno : r;
    }
    case UB_X32_sendmsg:
    case UB_sendmsg:
        return sys_msg(t, 1, a[0], (uint32_t)a[1], (int)a[2], nr == UB_X32_sendmsg);
    case UB_X32_recvmsg:
    case UB_recvmsg:
        return sys_msg(t, 0, a[0], (uint32_t)a[1], (int)a[2], nr == UB_X32_recvmsg);
    case UB_shutdown:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        RET(shutdown(h, (int)a[1]));
    case UB_getsockname:
    case UB_getpeername:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        if (nr == UB_getsockname)
            RET(getsockname(h, ros_ptr((uint32_t)a[1]), ros_ptr((uint32_t)a[2])));
        RET(getpeername(h, ros_ptr((uint32_t)a[1]), ros_ptr((uint32_t)a[2])));
    case UB_X32_setsockopt:
    case UB_setsockopt:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        RET(setsockopt(h, (int)a[1], (int)a[2], a[3] ? ros_ptr((uint32_t)a[3]) : NULL,
                       (socklen_t)a[4]));
    case UB_X32_getsockopt:
    case UB_getsockopt:
        if ((h = host_fd(t, a[0])) < 0)
            return h == -ENOTSUP ? -ENOTSOCK : h;
        RET(getsockopt(h, (int)a[1], (int)a[2], ros_ptr((uint32_t)a[3]),
                       ros_ptr((uint32_t)a[4])));
    case UB_select: {
        struct timeval *tv = a[4] ? ros_ptr((uint32_t)a[4]) : NULL;
        struct timespec ts, *tsp = NULL;
        if (tv) {
            if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
                return -EINVAL;
            ts = (struct timespec){ tv->tv_sec, tv->tv_usec * 1000 }, tsp = &ts;
        }
        return sys_select(t, a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], tsp);
    }
    case UB_pselect6:                   /* its signal mask is not applied: signals are not delivered yet */
        return sys_select(t, a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3],
                          a[4] ? ros_ptr((uint32_t)a[4]) : NULL);

    /* ---- paths */
    case UB_open: {
        PATH(0, p);
        int fd = openat(p_dir, p, (int)a[1] | O_CLOEXEC, (mode_t)a[2] & ~t->umask);
        return fd < 0 ? -errno : fd_install(t, fd, 0, !!(a[1] & O_CLOEXEC));
    }
    case UB_openat: {
        PATH(1, p);
        int fd = openat(dir_arg(t, a[0], p_dir), p, (int)a[2] | O_CLOEXEC,
                        (mode_t)a[3] & ~t->umask);
        return fd < 0 ? -errno : fd_install(t, fd, 0, !!(a[2] & O_CLOEXEC));
    }
    case UB_creat: {
        PATH(0, p);
        int fd = openat(p_dir, p, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC,
                        (mode_t)a[1] & ~t->umask);
        return fd < 0 ? -errno : fd_install(t, fd, 0, 0);
    }
    case UB_stat: {
        PATH(0, p);
        RET(fstatat(p_dir, p, ros_ptr((uint32_t)a[1]), 0));
    }
    case UB_lstat: {
        PATH(0, p);
        RET(fstatat(p_dir, p, ros_ptr((uint32_t)a[1]), AT_SYMLINK_NOFOLLOW));
    }
    case UB_newfstatat: {
        PATH(1, p);
        RET(fstatat(dir_arg(t, a[0], p_dir), p, ros_ptr((uint32_t)a[2]), (int)a[3]));
    }
    case UB_statx: {
        PATH(1, p);
        RET(syscall(SYS_statx, dir_arg(t, a[0], p_dir), p, (int)a[2], (unsigned)a[3],
                    ros_ptr((uint32_t)a[4])));
    }
    case UB_access: {
        PATH(0, p);
        RET(faccessat(p_dir, p, (int)a[1], 0));
    }
    case UB_faccessat:
    case UB_faccessat2: {
        PATH(1, p);
        RET(faccessat(dir_arg(t, a[0], p_dir), p, (int)a[2],
                      nr == UB_faccessat2 ? (int)a[3] : 0));
    }
    case UB_mkdir: {
        PATH(0, p);
        RET(mkdirat(p_dir, p, (mode_t)a[1] & ~t->umask));
    }
    case UB_mkdirat: {
        PATH(1, p);
        RET(mkdirat(dir_arg(t, a[0], p_dir), p, (mode_t)a[2] & ~t->umask));
    }
    case UB_unlink: {
        PATH(0, p);
        RET(unlinkat(p_dir, p, 0));
    }
    case UB_rmdir: {
        PATH(0, p);
        RET(unlinkat(p_dir, p, AT_REMOVEDIR));
    }
    case UB_unlinkat: {
        PATH(1, p);
        RET(unlinkat(dir_arg(t, a[0], p_dir), p, (int)a[2]));
    }
    case UB_rename: {
        PATH(0, p);
        PATH(1, q);
        RET(renameat(p_dir, p, q_dir, q));
    }
    case UB_renameat:
    case UB_renameat2: {
        PATH(1, p);
        PATH(3, q);
        RET(syscall(SYS_renameat2, dir_arg(t, a[0], p_dir), p, dir_arg(t, a[2], q_dir), q,
                    nr == UB_renameat2 ? (unsigned)a[4] : 0u));
    }
    case UB_readlink: {
        PATH(0, p);
        RET(readlinkat(p_dir, p, ros_ptr((uint32_t)a[1]), (size_t)a[2]));
    }
    case UB_readlinkat: {
        PATH(1, p);
        RET(readlinkat(dir_arg(t, a[0], p_dir), p, ros_ptr((uint32_t)a[2]), (size_t)a[3]));
    }
    case UB_chmod: {
        PATH(0, p);
        RET(fchmodat(p_dir, p, (mode_t)a[1], 0));
    }
    case UB_fchmodat: {
        PATH(1, p);
        RET(fchmodat(dir_arg(t, a[0], p_dir), p, (mode_t)a[2], 0));
    }
    case UB_fchmod:
        if ((h = host_fd(t, a[0])) < 0)
            return h;
        RET(fchmod(h, (mode_t)a[1]));
    case UB_utimensat: {
        if (!a[1]) {
            if ((h = host_fd(t, a[0])) < 0)
                return h;
            RET(futimens(h, a[2] ? ros_ptr((uint32_t)a[2]) : NULL));
        }
        PATH(1, p);
        RET(utimensat(dir_arg(t, a[0], p_dir), p, a[2] ? ros_ptr((uint32_t)a[2]) : NULL,
                      (int)a[3]));
    }
    case UB_chdir: {
        PATH(0, p);
        return sys_chdir(t, p_dir, p);
    }
    case UB_getcwd:
        return sys_getcwd(t, (uint32_t)a[0], (uint64_t)a[1]);
    case UB_umask: {
        unsigned old = t->umask;
        t->umask = (unsigned)a[0] & 0777;
        return old;
    }

    /* ---- memory */
    case UB_brk:
        return sys_brk(t, (uint64_t)a[0]);
    case UB_mmap:
        return sys_mmap(t, a);
    case UB_munmap:
        return sys_munmap(t, (uint64_t)a[0], (uint64_t)a[1]);
    case UB_mremap:
        return -ENOMEM;                 /* musl copies instead */
    case UB_mprotect:
    case UB_madvise:
        return in_heap(t, (uint64_t)a[0], (uint64_t)a[1]) ? 0 : -EINVAL;

    /* ---- the process */
    case UB_exit:                       /* a thread's end: the only thread's, so the process's */
    case UB_exit_group:
        sys_exit(t, (int)a[0]);
        return 0;
    case UB_getpid:
    case UB_gettid:
    case UB_set_tid_address:
        return t->pid;
    case UB_getppid:
        return 1;
    case UB_getuid:
    case UB_geteuid:
    case UB_getgid:
    case UB_getegid:
        return 0;
    case UB_X32_rt_sigaction:
    case UB_rt_sigaction: {
        int sig = (int)a[0];
        if (sig < 1 || sig > 64 || sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        uint32_t *old = a[2] ? ros_ptr((uint32_t)a[2]) : NULL;
        const uint32_t *act = a[1] ? ros_ptr((uint32_t)a[1]) : NULL;
        if (old)
            memcpy(old, t->sa[sig], 5 * sizeof(uint32_t));
        if (act)
            memcpy(t->sa[sig], act, 5 * sizeof(uint32_t));
        return 0;
    }
    case UB_rt_sigprocmask: {
        uint64_t *old = a[2] ? ros_ptr((uint32_t)a[2]) : NULL;
        const uint64_t *set = a[1] ? ros_ptr((uint32_t)a[1]) : NULL;
        if (old)
            *old = t->sigmask;
        if (set) {
            if (a[0] == SIG_BLOCK)
                t->sigmask |= *set;
            else if (a[0] == SIG_UNBLOCK)
                t->sigmask &= ~*set;
            else
                t->sigmask = *set;
        }
        return 0;
    }
    case UB_X32_sigaltstack:
    case UB_sigaltstack:
        return 0;
    case UB_kill:
        return a[0] == t->pid || a[0] == 0 ? sys_kill_self(t, (int)a[1]) : -ESRCH;
    case UB_tkill:
        return a[0] == t->pid ? sys_kill_self(t, (int)a[1]) : -ESRCH;
    case UB_tgkill:
        return a[1] == t->pid ? sys_kill_self(t, (int)a[2]) : -ESRCH;
    case UB_futex:
        return (a[1] & 0x7f) == 0 /* FUTEX_WAIT */ ? -EAGAIN : 0;   /* one thread */

    /* ---- time, identity, the system */
    case UB_clock_gettime:
        RET(clock_gettime((clockid_t)a[0], ros_ptr((uint32_t)a[1])));
    case UB_clock_getres:
        RET(clock_getres((clockid_t)a[0], a[1] ? ros_ptr((uint32_t)a[1]) : NULL));
    case UB_gettimeofday:
        RET(syscall(SYS_gettimeofday, a[0] ? ros_ptr((uint32_t)a[0]) : NULL, NULL));
    case UB_nanosleep: {
        int r;
        ROS_BLOCKING(r = nanosleep(ros_ptr((uint32_t)a[0]),
                                   a[1] ? ros_ptr((uint32_t)a[1]) : NULL));
        return r < 0 ? -errno : 0;
    }
    case UB_clock_nanosleep: {
        int r;
        ROS_BLOCKING(r = clock_nanosleep((clockid_t)a[0], (int)a[1], ros_ptr((uint32_t)a[2]),
                                         a[3] ? ros_ptr((uint32_t)a[3]) : NULL));
        return -r;
    }
    case UB_sched_yield:
        ROS_BLOCKING(sched_yield());
        return 0;
    case UB_uname:
        RET(syscall(SYS_uname, ros_ptr((uint32_t)a[0])));
    case UB_getrandom:
        RET(getrandom(ros_ptr((uint32_t)a[0]), (size_t)a[1], (unsigned)a[2]));
    case UB_sysinfo:
        RET(syscall(SYS_sysinfo, ros_ptr((uint32_t)a[0])));
    case UB_prlimit64:
        if (a[2])
            RET(syscall(SYS_prlimit64, 0, (int)a[1], NULL, ros_ptr((uint32_t)a[3])));
        return a[3] ? (int64_t)syscall(SYS_prlimit64, 0, (int)a[1], NULL,
                                       ros_ptr((uint32_t)a[3])) : 0;
    default:
        refuse_once(nr);
        return -ENOSYS;
    }
}

static void release_all(void)
{
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used)
            release(&tasks[i]);
}

#else   /* not Linux: a hosted build has no x32 programs to serve */

static int64_t syscall_x(uint32_t nr, int x32, int64_t *a)
{
    (void)nr, (void)x32, (void)a;
    return -ENOSYS;
}

static void release_all(void)
{
}

#endif

/* SWI Unix_Syscall: R0 the number, R1 -> six 64-bit arguments */
void ros_thunk_Unix_Syscall(struct ros_cpu *s)
{
    uint32_t n = s->r[0];
    int x32 = n & UNIX_X32_BIT ? 1 : 0;
    uint32_t nr = n & ~UNIX_X32_BIT;
    int64_t r;
    if (!s->r[1] || !ros_in_arena(ros_ptr(s->r[1]))) {
        r = -EFAULT;
    } else {
        int64_t a[6];
        memcpy(a, ros_ptr(s->r[1]), sizeof a);
        /* An x32 caller's pointers and sizes are 32-bit values, as its C
         * library passes them. x86-64's are whole. */
        r = syscall_x(nr, x32, a);
    }
    s->r[0] = (uint32_t)(uint64_t)r;
    s->r[1] = (uint32_t)((uint64_t)r >> 32);
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    release_all();
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module unixbridge_module = {
    .title = "UnixBridge",
    .help = "UnixBridge\t0.01 (27 Sep 2026) ROSGD native: Linux's system calls for RISC OS",
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = UNIX_SWI_CHUNK,
    .swi_thunks = ros_swi_thunks_Unix,
    .swi_names = ros_swi_names_Unix,
    .swi_prefix = "Unix",
};

__attribute__((constructor)) static void count(void)
{
    unixbridge_module.swi_count = ros_swi_count_Unix;
}
