/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* pty.c -- PTY, a native module: Linux programs on pseudo-terminals.
 *
 * RISC OS runs a command-line program in the desktop through TaskWindow.
 * The program's output is collected in a buffer and handed to its parent,
 * which draws it, a Wimp poll at a time. The parent's keys are handed back.
 * Nothing waits on anything. A Linux program, OpenSSH's ssh first of all,
 * needs the same from ROSGD, and a terminal as well. It reads from one and
 * writes a VT100's escape sequences to it. So this module opens a
 * pseudo-terminal and runs the program on its far side, as its session's
 * controlling terminal. It gives RISC OS non-blocking reads and writes of
 * the near side:
 *
 *   PTY_Open    R1 a command line, R2 columns, R3 rows -> R0 a handle
 *   PTY_Read    what the program has written, never waiting: R3 bytes
 *   PTY_Write   keys for it: R3 taken
 *   PTY_Status  R1 1 once it has ended, R2 its exit status, R3 waiting
 *   PTY_Resize  the terminal's new size, SIGWINCH to the program
 *   PTY_Close   SIGHUP, as a terminal closing sends; waited for; freed
 *
 * The command line's first word is a program on the box's PATH
 * (/usr/bin:/bin), or on the host's PATH when hosted. The rest are its
 * arguments, split at spaces, with "quoted" words kept whole. Its
 * environment is a terminal's: TERM (vt100, which term.c draws), LANG
 * (UTF-8, which term.c reads), HOME from the password file, and PATH.
 *
 * A caller that wants the terminal too, *SSH for one, has pty_terminal()
 * (term.c). It draws the program's output with VDU codes in the text window
 * and sends the program the keyboard until it ends.
 *
 * The program is a child of /init. The runtime's other threads keep running
 * while it forks, so the child does only what is safe between fork and exec.
 * The path is found, and the arguments and the environment are built, before
 * the fork. An exec that fails says why down a pipe that the exec closes.
 */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#include "pty.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"

#define MAXARGS 64

struct pty {
    struct pty *next;
    uint32_t handle;
    int fd;                             /* the master side, non-blocking */
    pid_t pid;
    int ended, status;                  /* reaped, and how */
    uint64_t ended_at;                  /* when, in milliseconds */
    int dry;                            /* the far side gone: EOF or EIO */
};

static struct pty *ptys;
static uint32_t next_handle = 1;

/* ---- the program ------------------------------------------------------------ */

/* The command line's words, in buf */
static int split(const char *cmd, char *buf, size_t size, char *argv[MAXARGS])
{
    int argc = 0;
    size_t n = 0;
    const char *t = cmd;
    while (*t >= ' ' && argc < MAXARGS - 1 && n < size - 1) {
        while (*t == ' ')
            t++;
        if (*t < ' ')
            break;
        argv[argc++] = buf + n;
        int quoted = *t == '"';
        if (quoted)
            t++;
        while (*t >= ' ' && (quoted ? *t != '"' : *t != ' ') && n < size - 1)
            buf[n++] = *t++;
        if (quoted && *t == '"')
            t++;
        buf[n++] = 0;
    }
    argv[argc] = NULL;
    return argc;
}

static const char *search_path(void)
{
    if (getpid() == 1)
        return "/usr/bin:/bin";
    const char *p = getenv("PATH");
    return p ? p : "/usr/bin:/bin";
}

/* The program's file: a name with a / as it is, or the first on PATH */
static int find_program(const char *name, char *out, size_t size)
{
    if (strchr(name, '/')) {
        snprintf(out, size, "%s", name);
        return access(out, X_OK) == 0 ? 0 : -1;
    }
    const char *p = search_path();
    while (*p) {
        size_t l = strcspn(p, ":");
        snprintf(out, size, "%.*s/%s", (int)l, p, name);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0)
            return 0;
        p += l + (p[l] == ':');
    }
    return -1;
}

static const char *home_dir(void)
{
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *pw->pw_dir)
        return pw->pw_dir;
    const char *h = getenv("HOME");
    return h ? h : "/";
}

static void set_size(int fd, int cols, int rows)
{
    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
    ioctl(fd, TIOCSWINSZ, &ws);
}

os_error *pty_open(const char *cmdline, int cols, int rows, struct pty **out)
{
    char words[4096], *argv[MAXARGS], path[1024];
    if (split(cmdline, words, sizeof words, argv) == 0)
        return ros_error(PTY_ERR_CANNOT_RUN, "No program to run");
    if (find_program(argv[0], path, sizeof path) < 0)
        return ros_error(PTY_ERR_CANNOT_RUN, "%s: not found", argv[0]);

    static char env_term[] = "TERM=vt100", env_lang[] = "LANG=C.UTF-8";
    char env_home[600], env_path[1024], env_user[128];
    snprintf(env_home, sizeof env_home, "HOME=%s", home_dir());
    snprintf(env_path, sizeof env_path, "PATH=%s", search_path());
    struct passwd *pw = getpwuid(getuid());
    snprintf(env_user, sizeof env_user, "USER=%s", pw ? pw->pw_name : "root");
    char *envp[] = { env_term, env_lang, env_home, env_path, env_user, NULL };

    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) {
        int err = errno;
        if (m >= 0)
            close(m);
        return ros_error(PTY_ERR_NO_PTY, "No pseudo-terminal: %s", strerror(err));
    }
    fcntl(m, F_SETFD, FD_CLOEXEC);
    char slave[128];
    snprintf(slave, sizeof slave, "%s", ptsname(m));
    int sfd = open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (sfd < 0) {
        int err = errno;
        close(m);
        return ros_error(PTY_ERR_NO_PTY, "No pseudo-terminal: %s: %s", slave, strerror(err));
    }
    set_size(sfd, cols, rows);
    struct termios t;                   /* a terminal's usual: canonical, echo, CR -> NL */
    if (tcgetattr(sfd, &t) == 0) {
        t.c_iflag |= ICRNL | IXON;
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN;
        t.c_cc[VERASE] = 0x7F;
        tcsetattr(sfd, TCSANOW, &t);
    }

    int report[2];                      /* the exec's failure, as errno */
    if (pipe(report) < 0) {
        int err = errno;
        close(sfd), close(m);
        return ros_error(PTY_ERR_CANNOT_RUN, "%s: %s", argv[0], strerror(err));
    }
    fcntl(report[0], F_SETFD, FD_CLOEXEC);
    fcntl(report[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid == 0) {
        /* the child: async-signal-safe calls only */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        for (int sig = 1; sig < 32; sig++)
            if (sig != SIGKILL && sig != SIGSTOP)
                sigaction(sig, &dfl, NULL);
        setsid();
        int s = open(slave, O_RDWR);    /* the session's controlling terminal */
        if (s < 0)
            s = sfd;
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0), dup2(s, 1), dup2(s, 2);
        execve(path, argv, envp);
        int err = errno;
        ssize_t w = write(report[1], &err, sizeof err);
        (void)w;
        _exit(127);
    }
    int fork_err = errno;
    close(sfd);
    close(report[1]);
    if (pid < 0) {
        close(report[0]), close(m);
        return ros_error(PTY_ERR_CANNOT_RUN, "%s: %s", argv[0], strerror(fork_err));
    }
    int err = 0;
    ssize_t got;
    ROS_BLOCKING(got = read(report[0], &err, sizeof err));
    close(report[0]);
    if (got == (ssize_t)sizeof err) {
        close(m);
        ROS_BLOCKING(waitpid(pid, NULL, 0));
        return ros_error(PTY_ERR_CANNOT_RUN, "%s: %s", argv[0], strerror(err));
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);

    struct pty *p = calloc(1, sizeof *p);
    if (!p) {
        kill(pid, SIGKILL);
        ROS_BLOCKING(waitpid(pid, NULL, 0));
        close(m);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    p->handle = next_handle++;
    p->fd = m;
    p->pid = pid;
    p->next = ptys;
    ptys = p;
    *out = p;
    return NULL;
}

/* Reaped yet? */
static void reap(struct pty *p, int wait)
{
    if (p->ended)
        return;
    int st;
    pid_t r;
    if (wait)
        ROS_BLOCKING(r = waitpid(p->pid, &st, 0));
    else
        r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid) {
        p->ended = 1;
        p->status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 0;
    } else if (r < 0 && errno == ECHILD) {
        p->ended = 1;                   /* reaped by someone else: status unknown */
    }
}

long pty_read(struct pty *p, void *buf, size_t n)
{
    if (p->dry || !n)
        return 0;
    ssize_t r = read(p->fd, buf, n);
    if (r > 0)
        return r;
    if (r == 0 || (errno != EAGAIN && errno != EINTR))
        p->dry = 1;                     /* EIO: the far side has no one left */
    return 0;
}

long pty_write(struct pty *p, const void *buf, size_t n)
{
    ssize_t w = write(p->fd, buf, n);
    return w < 0 ? 0 : w;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Ended once it has been reaped and what it wrote has been read. That is when
 * the far side is at its end (EIO). Or, when something it started holds the
 * far side open, it is when there has been nothing to read for a fifth of a
 * second. Linux moves a pseudo-terminal's output to the near side a moment
 * after the write, so an empty near side as the program exits is not yet the
 * end. */
int pty_ended(struct pty *p, int *status)
{
    reap(p, 0);
    if (status)
        *status = p->status;
    if (!p->ended)
        return 0;
    if (p->dry)
        return 1;
    int waiting = 0;
    if (ioctl(p->fd, FIONREAD, &waiting) == 0 && waiting > 0)
        return 0;
    if (!p->ended_at)
        p->ended_at = now_ms();
    return now_ms() - p->ended_at >= 200;
}

int pty_fd(struct pty *p)
{
    return p->fd;
}

int pty_close(struct pty *p)
{
    for (struct pty **q = &ptys; *q; q = &(*q)->next)
        if (*q == p) {
            *q = p->next;
            break;
        }
    if (!p->ended) {
        kill(p->pid, SIGHUP);
        reap(p, 0);
        for (int i = 0; i < 20 && !p->ended; i++) {     /* a second, then SIGKILL */
            ROS_BLOCKING(usleep(50000));
            reap(p, 0);
        }
        if (!p->ended) {
            kill(p->pid, SIGKILL);
            reap(p, 1);
        }
    }
    close(p->fd);
    int st = p->status;
    free(p);
    return st;
}

/* ---- the SWIs --------------------------------------------------------------- */

static struct pty *find(struct ros_cpu *s)
{
    for (struct pty *p = ptys; p; p = p->next)
        if (p->handle == s->r[0])
            return p;
    ros_swi_fail(s, ros_error(PTY_ERR_BAD_HANDLE, "Bad PTY handle"));
    return NULL;
}

static int size_ok(struct ros_cpu *s, uint32_t cols, uint32_t rows)
{
    if (cols >= 1 && cols <= 1000 && rows >= 1 && rows <= 1000)
        return 1;
    ros_swi_fail(s, ros_error(0x1EAu, "Bad parameters"));
    return 0;
}

void ros_thunk_PTY_Open(struct ros_cpu *s)
{
    if (!size_ok(s, s->r[2], s->r[3]))
        return;
    char line[1024];
    const char *c = ros_ptr(s->r[1]);
    size_t n = 0;
    while (n < sizeof line - 1 && (uint8_t)c[n] >= ' ')
        line[n] = c[n], n++;
    line[n] = 0;
    struct pty *p;
    os_error *e = pty_open(line, (int)s->r[2], (int)s->r[3], &p);
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    s->r[0] = p->handle;
}

void ros_thunk_PTY_Read(struct ros_cpu *s)
{
    struct pty *p = find(s);
    if (p)
        s->r[3] = (uint32_t)pty_read(p, ros_ptr(s->r[1]), s->r[2]);
}

void ros_thunk_PTY_Write(struct ros_cpu *s)
{
    struct pty *p = find(s);
    if (p)
        s->r[3] = (uint32_t)pty_write(p, ros_ptr(s->r[1]), s->r[2]);
}

void ros_thunk_PTY_Status(struct ros_cpu *s)
{
    struct pty *p = find(s);
    if (!p)
        return;
    int st, waiting = 0;
    s->r[1] = (uint32_t)pty_ended(p, &st);
    s->r[2] = (uint32_t)st;
    if (!p->dry && ioctl(p->fd, FIONREAD, &waiting) < 0)
        waiting = 0;
    s->r[3] = (uint32_t)waiting;
}

void ros_thunk_PTY_Resize(struct ros_cpu *s)
{
    struct pty *p = find(s);
    if (!p || !size_ok(s, s->r[2], s->r[3]))
        return;
    set_size(p->fd, (int)s->r[2], (int)s->r[3]);   /* the kernel sends SIGWINCH */
}

void ros_thunk_PTY_Close(struct ros_cpu *s)
{
    struct pty *p = find(s);
    if (p)
        s->r[2] = (uint32_t)pty_close(p);
}

/* ---- the module ------------------------------------------------------------- */

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    while (ptys)
        pty_close(ptys);
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module pty_module = {
    .title = "PTY",
    .help = "PTY\t1.00 (26 Sep 2026) ROSGD native: Linux programs on pseudo-terminals",
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0xC00C0,
    .swi_thunks = ros_swi_thunks_PTY,
    .swi_names = ros_swi_names_PTY,
    .swi_prefix = "PTY",
};

__attribute__((constructor)) static void count(void)
{
    pty_module.swi_count = ros_swi_count_PTY;
}
