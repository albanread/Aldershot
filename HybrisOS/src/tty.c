/* tty.c: pseudo-terminals, /dev/ptmx and /dev/pts/N, as on Linux.
 *
 * The box runs a program on a pseudo-terminal (see modules/pty, used by
 * *CC, *RunBox and *SSH). The box opens the master, and the program opens
 * the slave as its terminal. The box's terminal window reads what the
 * program writes, and writes what the user types. Between the two sides is
 * Linux's line discipline with its default settings. This file implements
 * enough of it for a program to behave as it does in a terminal:
 *
 *   Typed input (master to slave): CR becomes NL (ICRNL). In canonical
 *   mode the program receives a line when the line ends. Delete rubs out
 *   a character (ERASE), ^U rubs out the line (KILL), and ^D ends a line or
 *   the input (EOF). The line discipline echoes what is typed (ECHO). ^C sends SIGINT
 *   and ^\ sends SIGQUIT to the terminal's session (ISIG). Raw mode passes
 *   everything through as it arrives.
 *
 *   Written output (slave to master): NL becomes CR NL (OPOST, ONLCR).
 *
 * When every descriptor of the slave is closed, a read of the master gives
 * EIO once the master has read everything the program wrote. pty.c takes
 * this as the sign that the program has gone. When the master is closed,
 * the slave reads end of file and its session receives SIGHUP. */
#include "hal.h"

#define EIO    5
#define EAGAIN 11
#define EFAULT 14
#define EINVAL 22
#define ENOTTY 25
#define ENOSPC 28

#define PTYS 32
#define RING 8192

struct ring {
    unsigned head, count;
    char b[RING];
};

static struct pty {
    int used, masters, slaves, locked;
    struct ring in;                             /* to the program (the slave reads it) */
    struct ring out;                            /* from the program (the master reads it) */
    char line[1024];                            /* in canonical mode, the line being typed */
    unsigned line_len;
    unsigned lines;                             /* the number of complete lines in the in ring */
    unsigned eofs;                              /* ^Ds on an empty line, each a read of nothing */
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[19];
    uint16_t rows, cols, xpix, ypix;
    int session;                                /* the controlling process's pid */
    int slave_opened;
} ptys[PTYS];

#define ICRNL  0000400
#define INLCR  0000100
#define IGNCR  0000200
#define OPOST  0000001
#define ONLCR  0000004
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4

static uint64_t key(struct pty *p) { return (uint64_t)p; }
static void wake(struct pty *p)
{
    futex_wake(key(p), 1L << 30);
    futex_wake(KEY_POLL, 1L << 30);
}

static int put(struct ring *r, char c)
{
    if (r->count == RING)
        return 0;
    r->b[(r->head + r->count++) % RING] = c;
    return 1;
}

static char get(struct ring *r)
{
    char c = r->b[r->head];
    r->head = (r->head + 1) % RING, r->count--;
    return c;
}

/* Pass a character that the program wrote, or an echoed one, through output
 * processing and then to the master */
static void out(struct pty *p, char c)
{
    if ((p->oflag & OPOST) && (p->oflag & ONLCR) && c == '\n')
        put(&p->out, '\r');
    put(&p->out, c);
}

long tty_open_master(void)
{
    for (int i = 0; i < PTYS; i++) {
        struct pty *p = &ptys[i];
        if (p->used)
            continue;
        memset(p, 0, sizeof *p);
        p->used = 1, p->masters = 1, p->locked = 1;
        p->iflag = ICRNL | 02000;               /* ICRNL IXON */
        p->oflag = OPOST | ONLCR;
        p->cflag = 0277;                        /* B38400 CS8 CREAD */
        p->lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | 0100000 | 01000 | 04000;   /* IEXTEN ECHOCTL ECHOKE */
        static const uint8_t cc[19] = { 3, 0x1C, 0x7F, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1A, 0, 0x12, 0x0F, 0x17, 0x16, 0 };
        memcpy(p->cc, cc, sizeof cc);
        p->rows = 24, p->cols = 80;
        return i;
    }
    return -ENOSPC;
}

long tty_open_slave(int n)
{
    if (n < 0 || n >= PTYS || !ptys[n].used || ptys[n].locked || !ptys[n].masters)
        return -EIO;
    ptys[n].slaves++;
    ptys[n].slave_opened = 1;
    return n;
}

void tty_hold(int i, int master)
{
    if (master)
        ptys[i].masters++;
    else
        ptys[i].slaves++;
}

void tty_close(int i, int master)
{
    struct pty *p = &ptys[i];
    if (master) {
        if (--p->masters == 0 && p->session)
            proc_kill(p->session, 1);           /* SIGHUP */
    } else {
        p->slaves--;
    }
    wake(p);
    if (!p->masters && !p->slaves)
        p->used = 0;
}

/* ---- typed input: writes to the master -------------------------------- */

static void signal_session(struct pty *p, int sig)
{
    if (p->session)
        proc_kill(p->session, sig);
}

/* Pass the line typed so far to the reader as a line. A ^D on an empty line
 * instead gives an end of file (a read that returns nothing). */
static void line_done(struct pty *p, int eof)
{
    if (eof && !p->line_len) {
        p->eofs++;
        return;
    }
    for (unsigned i = 0; i < p->line_len; i++)
        put(&p->in, p->line[i]);
    p->line_len = 0;
    p->lines++;
}

long tty_master_write(int i, uint64_t buf, uint64_t n)
{
    struct pty *p = &ptys[i];
    uint64_t k = 0;
    for (; k < n; k++) {
        char c;
        if (copy_from_box(&c, buf + k, 1))
            return k ? (long)k : -EFAULT;
        if (p->lflag & ISIG) {
            if ((uint8_t)c == p->cc[VINTR] || (uint8_t)c == p->cc[VQUIT]) {
                p->line_len = 0;
                if (p->lflag & ECHO)
                    out(p, '^'), out(p, (char)(c + 64)), out(p, '\n');
                signal_session(p, (uint8_t)c == p->cc[VINTR] ? 2 : 3);
                continue;
            }
        }
        if (c == '\r') {
            if (p->iflag & IGNCR)
                continue;
            if (p->iflag & ICRNL)
                c = '\n';
        } else if (c == '\n' && (p->iflag & INLCR)) {
            c = '\r';
        }
        if (!(p->lflag & ICANON)) {
            if (p->in.count == RING)
                break;
            put(&p->in, c);
            if (p->lflag & ECHO)
                out(p, c);
            continue;
        }
        if ((uint8_t)c == p->cc[VERASE] || c == 8) {
            if (p->line_len) {
                p->line_len--;
                if ((p->lflag & ECHO) && (p->lflag & ECHOE))
                    out(p, 8), out(p, ' '), out(p, 8);
            }
            continue;
        }
        if ((uint8_t)c == p->cc[VKILL]) {
            while (p->line_len) {
                p->line_len--;
                if ((p->lflag & ECHO) && (p->lflag & ECHOK))
                    out(p, 8), out(p, ' '), out(p, 8);
            }
            continue;
        }
        if ((uint8_t)c == p->cc[VEOF]) {
            line_done(p, 1);
            continue;
        }
        if (p->line_len < sizeof p->line)
            p->line[p->line_len++] = c;
        if (p->lflag & ECHO)
            out(p, c);
        if (c == '\n')
            line_done(p, 0);
    }
    wake(p);
    return (long)k;
}

/* ---- the program's side: the slave ------------------------------------- */

long tty_slave_read(struct frame *fr, int i, uint64_t buf, uint64_t n, int nonblock)
{
    struct pty *p = &ptys[i];
    if (!p->masters)
        return 0;                               /* hung up, so end of file */
    int canon = (p->lflag & ICANON) != 0;
    if (canon && !p->lines && p->eofs) {
        p->eofs--;
        return 0;                               /* ^D gives end of file */
    }
    if (canon ? !p->lines : !p->in.count) {
        if (nonblock || !fr)
            return -EAGAIN;
        thread_block_restart(fr, key(p), 0);
        return SWITCHED;
    }
    /* Read a line (up to its newline, or as much as ^D sent) in canonical
     * mode, or whatever has arrived in raw mode */
    uint64_t k = 0;
    char c = 0;
    while (k < n && p->in.count) {
        c = get(&p->in);
        if (copy_to_box(buf + k, &c, 1))
            return k ? (long)k : -EFAULT;
        k++;
        if (canon && c == '\n')
            break;
    }
    if (canon && p->lines && (c == '\n' || !p->in.count))
        p->lines--;                             /* line read to its end (else the rest waits) */
    wake(p);
    return (long)k;
}

long tty_slave_write(struct frame *fr, int i, uint64_t buf, uint64_t n, int nonblock)
{
    struct pty *p = &ptys[i];
    if (!p->masters)
        return -EIO;
    if (p->out.count > RING - 4) {
        if (nonblock || !fr)
            return -EAGAIN;
        thread_block_restart(fr, key(p), 0);
        return SWITCHED;
    }
    uint64_t k = 0;
    while (k < n && p->out.count <= RING - 2) {
        char c;
        if (copy_from_box(&c, buf + k, 1))
            return k ? (long)k : -EFAULT;
        out(p, c);
        k++;
    }
    wake(p);
    return (long)k;
}

/* ---- the box's side: the master ---------------------------------------- */

long tty_master_read(struct frame *fr, int i, uint64_t buf, uint64_t n, int nonblock)
{
    struct pty *p = &ptys[i];
    if (!p->out.count) {
        if (p->slave_opened && !p->slaves)
            return -EIO;                        /* the program, and all it started, have gone */
        if (nonblock || !fr)
            return -EAGAIN;
        thread_block_restart(fr, key(p), 0);
        return SWITCHED;
    }
    uint64_t k = 0;
    while (k < n && p->out.count) {
        char c = get(&p->out);
        if (copy_to_box(buf + k, &c, 1))
            return k ? (long)k : -EFAULT;
        k++;
    }
    wake(p);
    return (long)k;
}

unsigned tty_poll(int i, int master)
{
    struct pty *p = &ptys[i];
    unsigned r = 0;
    if (master) {
        if (p->out.count)
            r |= 1;                             /* POLLIN */
        if (p->slave_opened && !p->slaves)
            r |= 1 | 16;                        /* POLLHUP: a read gives EIO */
        r |= 4;                                 /* POLLOUT */
    } else {
        if (((p->lflag & ICANON) ? p->lines || p->eofs : p->in.count) || !p->masters)
            r |= 1;
        if (!p->masters)
            r |= 16;
        r |= 4;
    }
    return r;
}

/* ---- ioctls --------------------------------------------------------------- */

long tty_ioctl(int i, int master, uint32_t req, uint64_t arg)
{
    struct pty *p = &ptys[i];
    int v;
    switch (req) {
    case 0x80045430:                            /* TIOCGPTN */
        if (!master)
            return -ENOTTY;
        v = i;
        return copy_to_box(arg, &v, 4) ? -EFAULT : 0;
    case 0x40045431:                            /* TIOCSPTLCK */
        if (copy_from_box(&v, arg, 4))
            return -EFAULT;
        p->locked = v != 0;
        return 0;
    case 0x5401: {                              /* TCGETS */
        uint8_t t[36];
        memset(t, 0, sizeof t);
        memcpy(t, &p->iflag, 4), memcpy(t + 4, &p->oflag, 4);
        memcpy(t + 8, &p->cflag, 4), memcpy(t + 12, &p->lflag, 4);
        memcpy(t + 17, p->cc, 19);
        return copy_to_box(arg, t, 36) ? -EFAULT : 0;
    }
    case 0x5402: case 0x5403: case 0x5404: {    /* TCSETS, TCSETSW, TCSETSF */
        uint8_t t[36];
        if (copy_from_box(t, arg, 36))
            return -EFAULT;
        memcpy(&p->iflag, t, 4), memcpy(&p->oflag, t + 4, 4);
        memcpy(&p->cflag, t + 8, 4), memcpy(&p->lflag, t + 12, 4);
        memcpy(p->cc, t + 17, 19);
        if (!(p->lflag & ICANON) && p->line_len)
            line_done(p, 0);                    /* now raw: pass on what was typed */
        wake(p);
        return 0;
    }
    case 0x5413: {                              /* TIOCGWINSZ */
        uint16_t w[4] = { p->rows, p->cols, p->xpix, p->ypix };
        return copy_to_box(arg, w, 8) ? -EFAULT : 0;
    }
    case 0x5414: {                              /* TIOCSWINSZ */
        uint16_t w[4];
        if (copy_from_box(w, arg, 8))
            return -EFAULT;
        int changed = w[0] != p->rows || w[1] != p->cols;
        p->rows = w[0], p->cols = w[1], p->xpix = w[2], p->ypix = w[3];
        if (changed)
            signal_session(p, 28);              /* SIGWINCH */
        return 0;
    }
    case 0x540E:                                /* TIOCSCTTY: caller session's terminal */
        p->session = thread_current()->proc->pid;
        return 0;
    case 0x5422:                                /* TIOCNOTTY */
        p->session = 0;
        return 0;
    case 0x540F: case 0x5429:                   /* TIOCGPGRP, TIOCGSID */
        v = p->session;
        return copy_to_box(arg, &v, 4) ? -EFAULT : 0;
    case 0x5410:                                /* TIOCSPGRP */
        return 0;
    case 0x541B:                                /* FIONREAD */
        v = (int)(master ? p->out.count : p->in.count);
        return copy_to_box(arg, &v, 4) ? -EFAULT : 0;
    case 0x540B:                                /* TCFLSH */
        if (arg != 1)
            p->in.count = 0, p->lines = 0, p->eofs = 0, p->line_len = 0;
        if (arg != 0)
            p->out.count = 0;
        return 0;
    case 0x5409: case 0x540A:                   /* TCSBRK, TCXONC */
        return 0;
    }
    return -ENOTTY;
}
