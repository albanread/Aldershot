/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* session.c -- a RISC OS command line per SSH session.
 *
 * sshd's login shell for root is riscos-cli (relay.c). That is /init under
 * another name. It connects the session's terminal to a Unix socket that
 * /init listens on. Each connection here is a session. Each session is a
 * RISC OS task of its own (task.h), with an application slot, an SVC stack
 * and the environment handlers. The task runs a command line: "*", a line
 * read, OS_CLI, and again. "ssh box <command>" runs the one command and ends.
 *
 * Only the task holding the baton runs, as under the Wimp. The box hands the
 * baton to a session whenever it has nothing of its own to do (the stream
 * hooks' idle, streams.h). That is in the console's task while it waits for
 * a key, and in the Wimp's poll while no task has an event. A session hands
 * the baton back when it wants input it does not have, and when it ends.
 * A session's task takes its own character streams. What it writes goes to
 * its terminal, with its VDU codes as ANSI's and its Latin-1 as UTF-8. What
 * it reads comes from its terminal. Escape (ESC on its own) raises the
 * escape condition, at once if the session is running. The socket is
 * watched as background work, which runs while a command waits. The
 * terminal's cursor-key sequences are dropped. Ctrl-D on an empty line,
 * Quit, Exit, Logout, or the connection closing ends the session.
 *
 * The protocol from riscos-cli to /init is a line "ROSGD-CLI 1 <I|C> <cols>
 * <rows>". For C, a line with the command follows. Then come the keys. From
 * /init to riscos-cli it is the terminal's bytes in UTF-8, so never &FF.
 * As the session ends, /init sends &FF and the exit status (0, or 1 after an
 * error).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/rma.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "sshd.h"

#define ESC_STATUS (ROS_ZEROPAGE + 0x104)     /* the kernel's ESC_Status */
#define ESC_CONDITION 0x40u

enum { S_HEADER, S_READY, S_RUNNING };
#define HEADER_MAX 4096                 /* most that a session's header may be */
enum { W_NONE, W_INPUT, W_INKEY };

struct session {
    struct session *next;
    uintptr_t serial;                   /* what the socket's watch names it by */
    int fd;
    int state;
    char mode;                          /* 'I' a command line, 'C' one command */
    int cols, rows;
    char *command;
    uint8_t *in;                        /* typed, not yet read */
    size_t in_n, in_cap;
    int eof, dead, escape;
    int waiting;
    uint64_t deadline;
    int esc_saved;                      /* its escape condition, while parked */
    int at_prompt, status;
    struct ros_task *task, *from;
    uint8_t out[4096];
    size_t out_n;
    uint32_t u8cp;                      /* a UTF-8 character being typed */
    int u8need;
    uint8_t vq[10];                     /* a VDU sequence being collected */
    int vneed, vn;
};

static struct session *sessions, *running;
static int listen_fd = -1;
static char listen_path[108];
static int sessions_ever;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int escape_set(void)
{
    return (ros_ld8(ESC_STATUS) & ESC_CONDITION) != 0;
}

static void escape_to(int on)
{
    uint32_t st = ros_ld8(ESC_STATUS);
    ros_st8(ESC_STATUS, on ? st | ESC_CONDITION : st & ~ESC_CONDITION);
}

/* The running session, if the running task is its */
static struct session *current(void)
{
    return running && ros_task_current() == running->task ? running : NULL;
}

/* The session whose task is running, however it got the baton. A Wimp task
 * that its command started is given the baton by the Wimp's switch as often
 * as by run(). What it writes is still the session's (#164) */
static struct session *of_task(void)
{
    struct ros_task *t = ros_task_current();
    if (!t)
        return NULL;
    for (struct session *s = sessions; s; s = s->next)
        if (s->task == t)
            return s;
    return NULL;
}

/* ---- output: VDU to ANSI --------------------------------------------------------- */

static void flush(struct session *s)
{
    size_t done = 0;
    while (done < s->out_n && !s->dead) {
        ssize_t w = send(s->fd, s->out + done, s->out_n - done, MSG_NOSIGNAL);
        if (w > 0) {
            done += (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = { .fd = s->fd, .events = POLLOUT };
            ROS_BLOCKING(poll(&p, 1, 1000));
        } else {
            s->dead = 1;                /* the terminal has gone: what it wrote is dropped */
        }
    }
    s->out_n = 0;
}

static void out(struct session *s, const char *b, size_t n)
{
    if (s->out_n + n > sizeof s->out)
        flush(s);
    memcpy(s->out + s->out_n, b, n);
    s->out_n += n;
}

static void outs(struct session *s, const char *b)
{
    out(s, b, strlen(b));
}

/* Acorn's characters at &80-&9F of RISC OS Latin-1 */
static const uint16_t acorn[32] = {
        0x20AC, 0x0174, 0x0175, 0, 0, 0x0176, 0x0177, 0, 0, 0, 0, 0,
        0x2026, 0x2122, 0x2030, 0x2022, 0x2018, 0x2019, 0x2039, 0x203A,
        0x201C, 0x201D, 0x201E, 0x2013, 0x2014, 0x2212, 0x0152, 0x0153,
        0x2020, 0x2021, 0xFB01, 0xFB02,
};

/* RISC OS Latin-1 to UTF-8 */
static void latin1(struct session *s, uint8_t c)
{
    uint32_t u = c < 0xA0 ? acorn[c - 0x80] : c;
    char b[3];
    if (!u) {
        out(s, "?", 1);
    } else if (u < 0x800) {
        b[0] = (char)(0xC0 | u >> 6), b[1] = (char)(0x80 | (u & 0x3F));
        out(s, b, 2);
    } else {
        b[0] = (char)(0xE0 | u >> 12), b[1] = (char)(0x80 | (u >> 6 & 0x3F));
        b[2] = (char)(0x80 | (u & 0x3F));
        out(s, b, 3);
    }
}

/* How many bytes follow each VDU code */
static const uint8_t vdu_args[32] = {
    [1] = 1, [17] = 1, [18] = 2, [19] = 5, [22] = 1, [23] = 9, [24] = 8, [25] = 5,
    [28] = 4, [29] = 4, [31] = 2,
};

static void vdu_done(struct session *s)
{
    char b[32];
    switch (s->vq[0]) {
    case 17: {                          /* COLOUR: 0-15 the foreground, +128 the background */
        static const uint8_t ansi[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        int c = s->vq[1], bg = c >= 128;
        c &= 15;
        snprintf(b, sizeof b, "\033[%dm", (bg ? (c < 8 ? 40 : 100) : (c < 8 ? 30 : 90)) + ansi[c & 7]);
        outs(s, b);
        break;
    }
    case 22:                            /* MODE: a clear screen */
        outs(s, "\033[0m\033[H\033[2J");
        break;
    case 23:
        if (s->vq[1] == 1)              /* the cursor on or off */
            outs(s, s->vq[2] ? "\033[?25h" : "\033[?25l");
        break;
    case 31:                            /* TAB(x, y) */
        snprintf(b, sizeof b, "\033[%d;%dH", s->vq[2] + 1, s->vq[1] + 1);
        outs(s, b);
        break;
    }
}

static void vdu(struct session *s, uint8_t c)
{
    if (s->vneed) {
        s->vq[s->vn++] = c;
        if (--s->vneed == 0)
            vdu_done(s);
        return;
    }
    if (c >= 32 && c < 127) {
        out(s, (const char *)&c, 1);
        return;
    }
    if (c >= 128) {
        latin1(s, c);
        return;
    }
    switch (c) {
    case 7: outs(s, "\a"); return;
    case 8: outs(s, "\b"); return;
    case 9: outs(s, "\033[C"); return;
    case 10: outs(s, "\n"); flush(s); return;
    case 11: outs(s, "\033[A"); return;
    case 12: outs(s, "\033[H\033[2J"); return;
    case 13: outs(s, "\r"); return;
    case 20: outs(s, "\033[0m"); return;
    case 30: outs(s, "\033[H"); return;
    case 127: outs(s, "\b \b"); return;
    }
    if (c < 32 && vdu_args[c]) {
        s->vq[0] = c, s->vn = 1, s->vneed = vdu_args[c];
    }
}

/* ---- input ------------------------------------------------------------------------ */

/* Park the session until idle() finds it something to do */
static void yield(struct session *s, int how)
{
    flush(s);
    s->waiting = how;
    s->esc_saved = escape_set();
    escape_to(0);
    running = NULL;
    ros_task_switch(s->from);
    s->waiting = W_NONE;
}

static int take(struct session *s)
{
    int c = s->in[0];
    memmove(s->in, s->in + 1, --s->in_n);
    return c;
}

static int hook_wrch(uint8_t c)
{
    struct session *s = of_task();
    if (!s)
        return 0;
    vdu(s, c);
    return 1;
}

static int hook_rdch(uint8_t *ch, int *escape)
{
    struct session *s = current();
    if (!s)
        return 0;
    for (;;) {
        if (escape_set()) {
            *ch = 27, *escape = 1;
            return 1;
        }
        if (s->in_n) {
            int c = take(s);
            if (c == 4 && s->at_prompt) {       /* Ctrl-D: the end */
                s->eof = 1;
                c = 13;
            }
            *ch = (uint8_t)c, *escape = 0;
            return 1;
        }
        if (s->eof || s->dead) {
            if (s->at_prompt) {
                *ch = 13, *escape = 0;
            } else {                    /* a program reading: stop it */
                ros_env_escape(1);
                *ch = 27, *escape = 1;
            }
            return 1;
        }
        yield(s, W_INPUT);
    }
}

static int hook_inkey(int cs)
{
    struct session *s = current();
    if (!s)
        return -2;
    uint64_t deadline = now_ms() + (uint64_t)cs * 10;
    for (;;) {
        if (s->in_n)
            return take(s);
        if (s->eof || s->dead || now_ms() >= deadline)
            return -1;
        s->deadline = deadline;
        yield(s, W_INKEY);
    }
}

/* ---- the sessions' tasks ------------------------------------------------------------ */

static int cli(const char *line)
{
    size_t n = strlen(line);
    char *c = ros_rma_alloc((uint32_t)n + 1);
    memcpy(c, line, n);
    c[n] = '\r';
    struct ros_cpu r;
    ros_cpu_enter(&r);
    r.r[0] = ros_addr(c);
    ros_swi(&r, XOS_CLI);
    ros_rma_free(c);
    if (!r.v)
        return 0;
    xos_write0(((os_error *)ros_ptr(r.r[0]))->errmess, NULL);
    xos_new_line();
    return 1;
}

static int is_end(const char *l)
{
    static const char *const words[] = { "quit", "exit", "logout" };
    while (*l == ' ' || *l == '*')
        l++;
    for (unsigned i = 0; i < 3; i++) {
        size_t n = strlen(words[i]);
        if (!strncasecmp(l, words[i], n) && (l[n] == 0 || l[n] == ' '))
            return 1;
    }
    return 0;
}

static void session_main(void *arg)
{
    struct session *s = arg;
    if (s->mode == 'C') {
        s->status = cli(s->command);
        flush(s);
        return;
    }
    char host[64] = "the box";
    gethostname(host, sizeof host);
    char banner[160];
    snprintf(banner, sizeof banner, "RISC OS on ROSGD -- a command line on %s. Quit, or Ctrl-D, ends it.",
             host);
    xos_write0(banner, NULL);
    xos_new_line();
    char *line = ros_rma_alloc(256);
    for (;;) {
        xos_write_c('*');
        s->at_prompt = 1;
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[0] = ros_addr(line), r.r[1] = 255, r.r[2] = ' ', r.r[3] = 255, r.r[4] = 0;
        ros_swi(&r, XOS_ReadLine32);
        s->at_prompt = 0;
        if (s->eof || s->dead)
            break;
        if (r.v || r.c) {               /* Escape: the condition is cleared (OS_Byte 124)
                                           and not acknowledged (126), which
                                           would flush the console's keyboard
                                           buffer */
            struct ros_cpu a;
            ros_cpu_enter(&a);
            a.r[0] = 124;
            ros_swi(&a, XOS_Byte);
            xos_new_line();
            xos_write0("Escape", NULL);
            xos_new_line();
            continue;
        }
        line[r.r[1]] = 0;
        if (is_end(line))
            break;
        s->status = cli(line);
    }
    ros_rma_free(line);
    flush(s);
}

static void finish(struct session *s)
{
    ros_unwatch(s->fd);
    if (!s->dead) {
        uint8_t end[2] = { 0xFF, (uint8_t)s->status };
        send(s->fd, end, 2, MSG_NOSIGNAL);
    }
    close(s->fd);
    for (struct session **p = &sessions; *p; p = &(*p)->next)
        if (*p == s) {
            *p = s->next;
            break;
        }
    if (s->task)
        ros_task_destroy(s->task);
    free(s->in);
    free(s->command);
    free(s);
}

/* The escape condition is the machine's. The caller's is kept aside while
 * the session runs, and given back after */
static void run(struct session *s)
{
    int caller = escape_set();
    s->from = ros_task_current();
    running = s;
    escape_to(s->esc_saved || s->escape);
    s->escape = 0;
    ros_task_switch(s->task);
    running = NULL;
    escape_to(caller);
}

static int runnable(const struct session *s)
{
    if (s->waiting == W_INPUT)
        return s->in_n || s->eof || s->dead || s->escape;
    if (s->waiting == W_INKEY)
        return s->in_n || s->eof || s->dead || s->escape || now_ms() >= s->deadline;
    return 0;
}

/* The box waits: run every session that has something to do */
static void hook_idle(void)
{
    static int busy;
    if (busy || running || !sessions)
        return;
    busy = 1;
    for (struct session *s = sessions, *next; s; s = next) {
        next = s->next;
        if (s->state == S_READY) {
            s->task = ros_task_create(ros_slot_next_size, session_main, s);
            if (!s->task) {
                s->status = 1;
                out(s, "No room for a session\r\n", 23);
                flush(s);
                finish(s);
                continue;
            }
            s->state = S_RUNNING;
            run(s);
        } else if (s->state == S_RUNNING && runnable(s)) {
            run(s);
        }
        if (s->state == S_RUNNING && ros_task_ended(s->task))
            finish(s);
        else if (s->state == S_HEADER && s->eof)
            finish(s);
    }
    busy = 0;
}

static int hook_owns(void)
{
    return of_task() != NULL;
}

static const struct ros_stream_hooks hooks = {
    .wrch = hook_wrch, .rdch = hook_rdch, .inkey = hook_inkey, .idle = hook_idle,
    .owns = hook_owns,
};

/* ---- the socket ------------------------------------------------------------------------ */

static void header(struct session *s)
{
    char *nl = memchr(s->in, '\n', s->in_n);
    if (!nl)
        return;
    char line[128];
    size_t n = (size_t)(nl - (char *)s->in);
    snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), (char *)s->in);
    char mode = 0;
    int cols = 80, rows = 24;
    if (sscanf(line, "ROSGD-CLI 1 %c %d %d", &mode, &cols, &rows) < 1 || (mode != 'I' && mode != 'C')) {
        s->eof = 1;                     /* not ours */
        return;
    }
    size_t used = n + 1;
    if (mode == 'C') {
        char *nl2 = memchr(s->in + used, '\n', s->in_n - used);
        if (!nl2)
            return;                     /* the command line is still coming */
        size_t cn = (size_t)(nl2 - (char *)s->in) - used;
        s->command = malloc(cn + 1);
        memcpy(s->command, s->in + used, cn);
        s->command[cn] = 0;
        used += cn + 1;
    }
    memmove(s->in, s->in + used, s->in_n - used);
    s->in_n -= used;
    s->mode = mode, s->cols = cols, s->rows = rows;
    s->state = S_READY;
}

/* A character typed, in RISC OS Latin-1: ? if it has none */
static uint8_t to_latin1(uint32_t u)
{
    if (u < 0x80 || (u >= 0xA0 && u <= 0xFF))
        return (uint8_t)u;
    for (int i = 0; i < 32; i++)
        if (acorn[i] == u)
            return (uint8_t)(0x80 + i);
    return '?';
}

/* What came, into the session's buffer. UTF-8 becomes RISC OS Latin-1. A
 * lone ESC is Escape, and the terminal's other escape sequences are dropped */
static void keys(struct session *s, const uint8_t *b, size_t n)
{
    if (s->state == S_HEADER && s->in_n + n > HEADER_MAX) {
        s->eof = 1;                     /* a header is a line or two: this one never ends */
        return;
    }
    if (s->in_n + n + 1 > s->in_cap) {              /* one over: a held UTF-8 lead can add a '?' */
        size_t cap = (s->in_n + n) * 2 + 256;
        uint8_t *m = realloc(s->in, cap);
        if (!m)
            return;
        s->in = m, s->in_cap = cap;
    }
    if (s->state == S_HEADER) {         /* the header is not keys */
        memcpy(s->in + s->in_n, b, n);
        s->in_n += n;
        header(s);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (s->u8need) {                /* a UTF-8 sequence's rest */
            if ((b[i] & 0xC0) == 0x80) {
                s->u8cp = s->u8cp << 6 | (b[i] & 0x3F);
                if (--s->u8need == 0)
                    s->in[s->in_n++] = to_latin1(s->u8cp);
                continue;
            }
            s->u8need = 0;
            s->in[s->in_n++] = '?';
        }
        if (b[i] >= 0xC2 && b[i] <= 0xF4) {
            s->u8cp = b[i] & (b[i] >= 0xF0 ? 0x07 : b[i] >= 0xE0 ? 0x0F : 0x1F);
            s->u8need = b[i] >= 0xF0 ? 3 : b[i] >= 0xE0 ? 2 : 1;
            continue;
        }
        if (b[i] != 27) {
            s->in[s->in_n++] = b[i] >= 0x80 ? '?' : b[i];
            continue;
        }
        if (i + 1 < n && (b[i + 1] == '[' || b[i + 1] == 'O')) {
            for (i += 2; i < n && !(b[i] >= 0x40 && b[i] <= 0x7E); i++)
                ;
            continue;
        }
        if (running == s)
            ros_env_escape(1);          /* its program told, as a key's Escape */
        else
            s->escape = 1;
    }
}

/* By its serial: a watch's work may come after the session has gone */
static struct session *find(uintptr_t serial)
{
    for (struct session *s = sessions; s; s = s->next)
        if (s->serial == serial)
            return s;
    return NULL;
}

static void on_data(void *arg, uint32_t revents)
{
    (void)revents;
    struct session *s = find((uintptr_t)arg);
    if (!s)
        return;
    uint8_t b[4096];
    for (;;) {
        ssize_t r = recv(s->fd, b, sizeof b, 0);
        if (r > 0) {
            keys(s, b, (size_t)r);
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        s->eof = 1;                     /* closed, or broken */
        return;
    }
    ros_watch(s->fd, POLLIN, on_data, (void *)s->serial);
}

static void on_accept(void *arg, uint32_t revents)
{
    (void)arg, (void)revents;
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            break;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        struct session *s = calloc(1, sizeof *s);
        if (!s) {
            close(fd);
            continue;
        }
        s->fd = fd;
        s->serial = (uintptr_t)++sessions_ever;
        s->next = sessions;
        sessions = s;
        ros_watch(fd, POLLIN, on_data, (void *)s->serial);
    }
    ros_watch(listen_fd, POLLIN, on_accept, NULL);
}

int sshd_listen(const char *path)
{
    if (listen_fd >= 0)
        return strcmp(path, listen_path) ? -EBUSY : 0;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof a.sun_path)
        return -ENAMETOOLONG;
    strcpy(a.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    unlink(path);
    mode_t old = umask(0177);           /* a socket made at 0600, not opened up until chmod */
    int bound = bind(fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (bound < 0 || listen(fd, 16) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    chmod(path, 0600);
    listen_fd = fd;
    snprintf(listen_path, sizeof listen_path, "%s", path);
    ros_stream_hooks = &hooks;
    ros_watch(fd, POLLIN, on_accept, NULL);
    return 0;
}

void sshd_unlisten(void)
{
    if (listen_fd < 0)
        return;
    ros_unwatch(listen_fd);
    close(listen_fd);
    unlink(listen_path);
    listen_fd = -1;
}

int sshd_session_count(void)
{
    int n = 0;
    for (struct session *s = sessions; s; s = s->next)
        n++;
    return n;
}

int sshd_sessions_ever(void)
{
    return sessions_ever;
}
