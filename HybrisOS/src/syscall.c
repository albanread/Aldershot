/* syscall.c: the system call layer.  The box's musl library makes Linux
 * system calls, and the HAL answers them here, as musl's back end.
 *
 * The HAL answers about 150 of Linux's calls: the ones that /init and the
 * programs it starts (clang, ssh, sshd and the rest) use.  It also answers
 * four calls of its own: HAL_SYS_SCREEN, HAL_SYS_AUDIO, HAL_SYS_FLUSH, and
 * HAL_SYS_STATS for DeskMeter.  Linux refuses these with ENOSYS, so the same
 * /init runs on both.  The HAL reports any call it does not know once on
 * the console, with its number, and refuses it with ENOSYS. */
#include "hal.h"

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ESPIPE 29
#define ENOSYS 38
#define EAFNOSUPPORT 97
#define EPIPE 32
#define ETIMEDOUT 110


/* ---- file descriptors ------------------------------------------------- */

enum { FD_FREE, FD_CONSOLE, FD_MEMFD, FD_MEMFILE, FD_PIPE_R, FD_PIPE_W, FD_EVENTFD,
       FD_INPUT, FD_INPUT_DIR,       /* /dev/input/eventN (virtio.c), and /dev/input itself */
       FD_FILE,                      /* a file or directory on the share or the RAM disc (vfs.c) */
       FD_SOCKET,                    /* a socket (socket.c) */
       FD_AUDIO,                     /* the sound device (sound.c) */
       FD_PTM, FD_PTS,               /* the master and slave ends of a pseudo-terminal (tty.c) */
       FD_DEV,                       /* /dev/null (obj 0), zero (1), urandom and random (2), full (3) */
       FD_UNIX };                    /* an AF_UNIX stream socket (unix.c) */
#define FDS 4096
struct fd {
    int type;
    int obj;                                    /* the memfd object, or the pipe or eventfd index */
    const char *data;                           /* FD_MEMFILE: the file's text */
    uint64_t size, pos;
    int cloexec;
    int nonblock;                               /* O_NONBLOCK: pipe, eventfd or console */
};
/* The running process's descriptor table (process.c keeps one for each process). */
#define fds ((struct fd *)thread_current()->proc->fdt)

int memfd_obj_new(void);
long memfd_obj_truncate(int obj, uint64_t size);

int memfd_obj_of_fd(int fd)
{
    return fd >= 0 && fd < FDS && fds[fd].type == FD_MEMFD ? fds[fd].obj : -1;
}

static int fd_new(int type)
{
    for (int i = 0; i < FDS; i++)
        if (fds[i].type == FD_FREE) {
            memset(&fds[i], 0, sizeof fds[i]);
            fds[i].type = type;
            return i;
        }
    return -EMFILE;
}

static int fd_ok(long fd);

/* Returns a descriptor for socket object o (socket.c), or -EMFILE, in which
 * case o is closed. */
static long new_socket_fd(long o)
{
    int fd = fd_new(FD_SOCKET);
    if (fd < 0)
        sock_close((int)o);
    else
        fds[fd].obj = (int)o;
    return fd;
}

static int fd_ok(long fd)
{
    return fd >= 0 && fd < FDS && fds[fd].type != FD_FREE;
}

/* ---- pipes and eventfds ------------------------------------------------ */

#define PIPES 32
static struct pipe {
    int used, readers, writers;
    unsigned head, count;
    char buf[4096];
} pipes[PIPES];
static struct eventfd {
    int used, refs;
    uint64_t value;
} eventfds[PIPES];

static uint64_t pipe_key(int i) { return (uint64_t)&pipes[i]; }
static uint64_t event_key(int i) { return (uint64_t)&eventfds[i]; }

static void hold_fd(struct fd *f)               /* one more descriptor on the object */
{
    if (f->type == FD_PIPE_R)
        pipes[f->obj].readers++;
    else if (f->type == FD_PIPE_W)
        pipes[f->obj].writers++;
    else if (f->type == FD_EVENTFD)
        eventfds[f->obj].refs++;
    else if (f->type == FD_FILE)
        vfs_hold(f->obj);
    else if (f->type == FD_SOCKET)
        sock_hold(f->obj);
    else if (f->type == FD_PTM || f->type == FD_PTS)
        tty_hold(f->obj, f->type == FD_PTM);
    else if (f->type == FD_UNIX)
        unix_hold(f->obj);
}

static void hold(int fd)
{
    hold_fd(&fds[fd]);
}

static void release_fd(struct fd *f)
{
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) {
        struct pipe *p = &pipes[f->obj];
        if (f->type == FD_PIPE_R)
            p->readers--;
        else
            p->writers--;
        futex_wake(pipe_key(f->obj), 1L << 30);
        futex_wake(KEY_POLL, 1L << 30);
        if (!p->readers && !p->writers)
            p->used = 0;
    } else if (f->type == FD_EVENTFD && --eventfds[f->obj].refs == 0) {
        eventfds[f->obj].used = 0;
    } else if (f->type == FD_SOCKET) {
        sock_close(f->obj);
    } else if (f->type == FD_PTM || f->type == FD_PTS) {
        tty_close(f->obj, f->type == FD_PTM);
    } else if (f->type == FD_UNIX) {
        unix_close(f->obj);
    } else if (f->type == FD_FILE) {
        vfs_close(f->obj);
    }
    f->type = FD_FREE;
}

static void release(int fd)
{
    release_fd(&fds[fd]);
}

/* ---- descriptor tables, one for each process ---------------------------- */

#define FDT_PAGES ((FDS * sizeof(struct fd) + PAGE_MASK) >> 12)

/* Makes a new table.  It is either empty or a copy of the table at from,
 * taking a new reference on each descriptor.  Fork and vfork use the copy
 * for the child. */
void *fdt_new(void *from)
{
    uint64_t pa = pages_alloc_run(FDT_PAGES);
    if (!pa)
        return 0;
    struct fd *t = pa_to_va(pa);
    if (from) {
        memcpy(t, from, FDS * sizeof(struct fd));
        for (int i = 0; i < FDS; i++)
            if (t[i].type != FD_FREE)
                hold_fd(&t[i]);
    }
    return t;
}

void fdt_close_all(void *fdt)
{
    struct fd *t = fdt;
    for (int i = 0; i < FDS; i++)
        if (t[i].type != FD_FREE)
            release_fd(&t[i]);
}

void fdt_free(void *fdt)
{
    uint64_t pa = va_to_pa(fdt);
    for (uint64_t i = 0; i < FDT_PAGES; i++)
        page_free(pa + i * PAGE_SIZE);
}

void fdt_exec(void *fdt)
{
    struct fd *t = fdt;
    for (int i = 0; i < FDS; i++)
        if (t[i].type != FD_FREE && t[i].cloexec)
            release_fd(&t[i]);
}

/* ---- console input: bytes from the PL011, kept until they are read ------ */

static char cin[256];
static unsigned cin_head, cin_count;

/* Called at each tick and before a read.  Moves the UART's bytes into cin
 * and wakes any thread waiting for them. */
int console_wakeup(void)
{
    int got = 0, c;
    while (cin_count < sizeof cin && (c = uart_getc()) >= 0) {
        if (c == 0x1C) {                        /* Ctrl-\ prints the HAL's thread dump */
            void thread_dump(void);
            thread_dump();
            continue;
        }
        cin[(cin_head + cin_count++) % sizeof cin] = (char)(c == '\r' ? '\n' : c);
        got++;
    }
    if (got) {
        futex_wake(KEY_CONSOLE, 1L << 30);
        futex_wake(KEY_POLL, 1L << 30);
    }
    return got;
}

/* Small files that the HAL makes itself: /proc/cmdline and /proc/meminfo. */
static char cmdline_text[1024], meminfo_text[256];

static void num(char **p, uint64_t v)
{
    char b[24];
    int n = 0;
    do
        b[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *(*p)++ = b[--n];
}

static void cat(char **p, const char *s)
{
    while (*s)
        *(*p)++ = *s++;
}

static const char *make_meminfo(void)
{
    char *p = meminfo_text;
    uint64_t total = boot.ram_size >> 10, free = pages_free() * 4;
    cat(&p, "MemTotal:       "), num(&p, total), cat(&p, " kB\n");
    cat(&p, "MemFree:        "), num(&p, free), cat(&p, " kB\n");
    cat(&p, "MemAvailable:   "), num(&p, free), cat(&p, " kB\n");
    *p = 0;
    return meminfo_text;
}

#define AT_FDCWD (-100)

/* Turns a path from the box into a full path.  It takes a relative path
 * relative to dirfd (a directory on the share) or otherwise to the
 * process's current directory. */
static long box_path(long dirfd, uint64_t va, char *out, size_t max)
{
    char p[512];
    long n = strlen_box(va, sizeof p - 1);
    if (n < 0)
        return -EFAULT;
    if (n == (long)sizeof p - 1)
        return -36;                             /* ENAMETOOLONG */
    copy_from_box(p, va, (size_t)n + 1);
    const char *dir = thread_current()->proc->cwd;
    if (!strcmp(dir, "/"))
        dir = "";
    if (p[0] != '/' && dirfd != AT_FDCWD && fd_ok(dirfd) && fds[dirfd].type == FD_FILE)
        dir = vfs_path(fds[dirfd].obj);
    else if (p[0] != '/' && dirfd != AT_FDCWD && !fd_ok(dirfd))
        return -EBADF;
    size_t dl = strlen(dir);
    if (dl + (size_t)n + 2 > max)
        return -36;
    memcpy(out, dir, dl);
    if (p[0] != '/')
        out[dl++] = '/';
    memcpy(out + dl, p, (size_t)n + 1);
    return 0;
}

/* Returns the text of one of the HAL's own /proc files, or 0 if there is no such file. */
static const char *hal_text(const char *path)
{
    if (!strcmp(path, "/proc/cmdline")) {
        char *p = cmdline_text;
        size_t l = strlen(boot.bootargs);
        if (l > sizeof cmdline_text - 2)
            l = sizeof cmdline_text - 2;
        memcpy(p, boot.bootargs, l);
        p[l] = '\n', p[l + 1] = 0;
        return cmdline_text;
    }
    if (!strcmp(path, "/proc/meminfo"))
        return make_meminfo();
    return proc_text(path);                     /* the rest are in proc.c */
}

static long sys_openat(long dirfd, uint64_t path_va, long flags, long mode)
{
    char path[768];
    long e = box_path(dirfd, path_va, path, sizeof path);
    if (e)
        return e;
    char canon[1024];
    if ((e = vfs_resolve(path, canon, !(flags & 0100000))))       /* O_NOFOLLOW */
        return e;
    if (strlen(canon) >= sizeof path)
        return -36;
    memcpy(path, canon, strlen(canon) + 1);
    static const char *const devs[] = { "/dev/null", "/dev/zero", "/dev/urandom", "/dev/full", "/dev/random" };
    for (int i = 0; i < 5; i++)
        if (!strcmp(path, devs[i])) {
            int fd = fd_new(FD_DEV);
            if (fd >= 0)
                fds[fd].obj = i == 4 ? 2 : i;
            return fd;
        }
    if (!strcmp(path, "/dev/ptmx") || !strncmp(path, "/dev/pts/", 9)) {
        long t = path[5] == 'p' && path[8] == 'x' ? tty_open_master() : -1;
        if (path[8] == '/') {
            int n = 0;
            for (const char *q = path + 9; *q >= '0' && *q <= '9'; q++)
                n = n * 10 + (*q - '0');
            t = tty_open_slave(n);
        }
        if (t < 0)
            return t;
        int fd = fd_new(path[8] == 'x' ? FD_PTM : FD_PTS);
        if (fd < 0) {
            tty_close((int)t, path[8] == 'x');
            return fd;
        }
        fds[fd].obj = (int)t;
        fds[fd].nonblock = (flags & 04000) != 0;
        return fd;
    }
    if (!strcmp(path, "/dev/console") || !strcmp(path, "/dev/tty")) {
        return fd_new(FD_CONSOLE);
    }
    if (!strcmp(path, "/dev/input") || !strcmp(path, "/dev/input/"))
        return fd_new(FD_INPUT_DIR);
    if (!strncmp(path, "/dev/input/event", 16)) {
        const char *p = path + 16;
        int i = 0;
        while (*p >= '0' && *p <= '9')
            i = i * 10 + (*p++ - '0');
        if (*p || p == path + 16 || i >= input_devices())
            return -ENOENT;
        int fd = fd_new(FD_INPUT);
        if (fd >= 0)
            fds[fd].obj = i;
        return fd;
    }
    const char *text = hal_text(path);
    if (text && (flags & 3))
        return -13;                             /* EACCES: the HAL's own files are read-only */
    if (!text) {                                /* a file on the share or the RAM file system */
        long h = vfs_open(path, flags, mode);
        if (h < 0)
            return h;
        int fd = fd_new(FD_FILE);
        if (fd < 0)
            vfs_close(h);
        else
            fds[fd].obj = (int)h;
        return fd;
    }
    int fd = fd_new(FD_MEMFILE);
    if (fd < 0)
        return fd;
    fds[fd].data = text;
    fds[fd].size = strlen(text);
    return fd;
}

static long sys_getrandom(uint64_t buf, uint64_t n);

static long sys_read(struct frame *fr, long fd, uint64_t buf, uint64_t n)
{
    if (!fd_ok(fd))
        return -EBADF;
    struct fd *f = &fds[fd];
    if (f->type == FD_MEMFILE) {
        uint64_t left = f->size - f->pos;
        if (n > left)
            n = left;
        if (copy_to_box(buf, f->data + f->pos, n))
            return -EFAULT;
        f->pos += n;
        return (long)n;
    }
    if (f->type == FD_INPUT)
        return input_read(f->obj, buf, n);      /* always opened O_NONBLOCK */
    if (f->type == FD_FILE)
        return vfs_read(f->obj, buf, n, -1);
    if (f->type == FD_SOCKET)
        return sock_recv(fr, f->obj, buf, n, 0, 0, 0);
    if (f->type == FD_UNIX)
        return unix_recv(fr, f->obj, buf, n, 0);
    /* /dev/null gives nothing, zero gives zeros, urandom gives random bytes */
    if (f->type == FD_DEV) {
        if (f->obj == 0)
            return 0;
        if (f->obj == 2)
            return sys_getrandom(buf, n);
        static const unsigned char zero[256];
        for (uint64_t k = 0; k < n; k += sizeof zero)
            if (copy_to_box(buf + k, zero, n - k < sizeof zero ? n - k : sizeof zero))
                return -EFAULT;
        return (long)n;
    }
    if (f->type == FD_PTM)
        return tty_master_read(fr, f->obj, buf, n, f->nonblock);
    if (f->type == FD_PTS)
        return tty_slave_read(fr, f->obj, buf, n, f->nonblock);
    if (f->type == FD_CONSOLE) {
        console_wakeup();
        if (!cin_count) {
            if (f->nonblock)
                return -EAGAIN;
            if (!fr)
                return 0;
            thread_block_restart(fr, KEY_CONSOLE, 0);
            return SWITCHED;
        }
        uint64_t k = 0;
        while (k < n && cin_count) {
            if (copy_to_box(buf + k, &cin[cin_head], 1))
                return k ? (long)k : -EFAULT;
            cin_head = (cin_head + 1) % sizeof cin, cin_count--, k++;
        }
        return (long)k;
    }
    if (f->type == FD_PIPE_R) {
        struct pipe *p = &pipes[f->obj];
        if (!p->count) {
            if (!p->writers || !fr)
                return 0;                       /* end of file, or a later buffer of a readv */
            if (f->nonblock)
                return -EAGAIN;
            thread_block_restart(fr, pipe_key(f->obj), 0);
            return SWITCHED;
        }
        uint64_t k = 0;
        while (k < n && p->count) {
            if (copy_to_box(buf + k, &p->buf[p->head], 1))
                break;
            p->head = (p->head + 1) % sizeof p->buf, p->count--, k++;
        }
        futex_wake(pipe_key(f->obj), 1L << 30);   /* wake a writer waiting for room */
        futex_wake(KEY_POLL, 1L << 30);
        return (long)k;
    }
    if (f->type == FD_EVENTFD) {
        struct eventfd *e = &eventfds[f->obj];
        if (n < 8)
            return -EINVAL;
        if (!e->value) {
            if (f->nonblock)
                return -EAGAIN;
            if (!fr)
                return 0;
            thread_block_restart(fr, event_key(f->obj), 0);
            return SWITCHED;
        }
        uint64_t v = e->value;
        e->value = 0;
        return copy_to_box(buf, &v, 8) ? -EFAULT : 8;
    }
    return -EINVAL;
}

static long sys_write(struct frame *fr, long fd, uint64_t buf, uint64_t n)
{
    if (!fd_ok(fd))
        return -EBADF;
    struct fd *f = &fds[fd];
    if (f->type == FD_FILE)
        return vfs_write(f->obj, buf, n, -1);
    if (f->type == FD_SOCKET)
        return sock_send(fr, f->obj, buf, n, 0x4000, 0, 0);    /* flags: MSG_NOSIGNAL */
    if (f->type == FD_UNIX)
        return unix_send(fr, f->obj, buf, n, 0);
    if (f->type == FD_AUDIO)
        return sound_write(f->nonblock ? 0 : fr, buf, n);
    if (f->type == FD_DEV)
        return f->obj == 3 ? -28 : (long)n;    /* full gives ENOSPC; the others accept everything */
    if (f->type == FD_PTM)
        return tty_master_write(f->obj, buf, n);
    if (f->type == FD_PTS)
        return tty_slave_write(fr, f->obj, buf, n, f->nonblock);
    if (f->type == FD_PIPE_W) {
        struct pipe *p = &pipes[f->obj];
        if (!p->readers)
            return -EPIPE;
        if (p->count == sizeof p->buf) {
            if (!fr || f->nonblock)
                return -EAGAIN;
            thread_block_restart(fr, pipe_key(f->obj), 0);
            return SWITCHED;
        }
        uint64_t k = 0;
        while (k < n && p->count < sizeof p->buf) {
            char c;
            if (copy_from_box(&c, buf + k, 1))
                break;
            p->buf[(p->head + p->count++) % sizeof p->buf] = c;
            k++;
        }
        futex_wake(pipe_key(f->obj), 1L << 30);
        futex_wake(KEY_POLL, 1L << 30);
        return (long)k;
    }
    if (f->type == FD_EVENTFD) {
        uint64_t v;
        if (n < 8 || copy_from_box(&v, buf, 8))
            return -EINVAL;
        eventfds[f->obj].value += v;
        futex_wake(event_key(f->obj), 1L << 30);
        futex_wake(KEY_POLL, 1L << 30);
        return 8;
    }
    if (f->type != FD_CONSOLE)
        return -EINVAL;
    char b[256];
    for (uint64_t done = 0; done < n;) {
        uint64_t k = n - done > sizeof b ? sizeof b : n - done;
        if (copy_from_box(b, buf + done, k))
            return done ? (long)done : -EFAULT;
        uart_write(b, k);
        done += k;
    }
    return (long)n;
}

/* readv: reads into each buffer in turn, and stops at the first one that is
 * not filled.  Only the read into the first buffer may wait. */
static long sys_readv(struct frame *fr, long fd, uint64_t iov, long cnt)
{
    long total = 0;
    for (long i = 0; i < cnt; i++) {
        uint64_t v[2];
        if (copy_from_box(v, iov + (uint64_t)i * 16, 16))
            return total ? total : -EFAULT;
        if (!v[1])
            continue;
        long r = sys_read(total ? 0 : fr, fd, v[0], v[1]);
        if (r == SWITCHED)
            return SWITCHED;
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < v[1])
            break;
    }
    return total;
}

/* ppoll: reports the descriptors that are ready now.  If none is ready, the
 * thread waits (for input, a pipe, an eventfd or the timeout) and then looks
 * again. */
#define POLLIN 1
#define POLLOUT 4
#define POLLHUP 0x10
#define POLLNVAL 0x20

static unsigned poll_ready(int fd, unsigned events)
{
    if (!fd_ok(fd))
        return POLLNVAL;
    struct fd *f = &fds[fd];
    unsigned r = 0;
    switch (f->type) {
    case FD_CONSOLE:
        console_wakeup();
        r = (cin_count ? POLLIN : 0) | POLLOUT;
        break;
    case FD_PIPE_R: {
        struct pipe *p = &pipes[f->obj];
        r = (p->count ? POLLIN : 0) | (!p->writers ? POLLHUP : 0);
        break;
    }
    case FD_PIPE_W: {
        struct pipe *p = &pipes[f->obj];
        r = (p->count < sizeof p->buf ? POLLOUT : 0) | (!p->readers ? POLLHUP : 0);
        break;
    }
    case FD_EVENTFD:
        r = (eventfds[f->obj].value ? POLLIN : 0) | POLLOUT;
        break;
    case FD_INPUT:
        r = input_pending(f->obj) ? POLLIN : 0;
        break;
    case FD_SOCKET:
        r = sock_poll(f->obj);
        break;
    case FD_PTM: case FD_PTS:
        r = tty_poll(f->obj, f->type == FD_PTM);
        break;
    case FD_UNIX:
        r = unix_poll(f->obj);
        break;
    default:
        r = POLLIN | POLLOUT;
    }
    return r & (events | POLLHUP | POLLNVAL);
}

static uint64_t boot_seconds, boot_counter;

static long sys_ppoll(struct frame *fr, uint64_t pfds, uint64_t n, uint64_t tsp, uint64_t sigmask)
{
    struct thread *t = thread_current();
    if (sigmask && !t->mask_restore && !t->poll_deadline) {     /* mask for the wait */
        uint64_t m;
        if (copy_from_box(&m, sigmask, 8))
            return -EFAULT;
        t->saved_mask = t->sigmask;
        t->sigmask = m & ~((1UL << 8) | (1UL << 18));
        t->mask_restore = 1;
    }
    if (t->pending & ~t->sigmask) {             /* an unmasked signal arrived before it waited */
        t->poll_deadline = 0;
        fr->x[0] = (uint64_t)-EINTR;
        schedule(fr);                           /* load() delivers it, with ppoll's mask in force */
        return SWITCHED;
    }
    long ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        int32_t pf[2];                          /* fd, then events and revents */
        if (copy_from_box(pf, pfds + i * 8, 8))
            return -EFAULT;
        unsigned rev = pf[0] < 0 ? 0 : poll_ready(pf[0], (unsigned)pf[1] & 0xFFFF);
        pf[1] = (pf[1] & 0xFFFF) | (int32_t)(rev << 16);
        copy_to_box(pfds + i * 8, pf, 8);
        ready += rev != 0;
    }
    uint64_t freq = counter_freq(), now = counter_read();
    if (!t->poll_deadline) {                    /* the first look, not a look after waking */
        if (tsp) {
            uint64_t ts[2];
            if (copy_from_box(ts, tsp, 16))
                return -EFAULT;
            t->poll_deadline = now + ts[0] * freq + ts[1] * freq / 1000000000UL;
        } else {
            t->poll_deadline = ~0UL;
        }
    }
    if (ready || (t->poll_deadline != ~0UL && (int64_t)(now - t->poll_deadline) >= 0)) {
        t->poll_deadline = 0;
        return ready;
    }
    thread_block_restart(fr, KEY_POLL, t->poll_deadline == ~0UL ? 0 : t->poll_deadline);
    return SWITCHED;
}

static long sys_writev(long fd, uint64_t iov, long cnt)
{
    long total = 0;
    for (long i = 0; i < cnt; i++) {
        uint64_t v[2];
        if (copy_from_box(v, iov + (uint64_t)i * 16, 16))
            return -EFAULT;
        long r = sys_write(0, fd, v[0], v[1]);   /* no frame is passed, so it does not wait */
        if (r < 0)
            return total ? total : r;
        total += r;
    }
    return total;
}

/* ---- time ------------------------------------------------------------- */

static void now(int clock, uint64_t *sec, uint64_t *nsec)
{
    uint64_t f = counter_freq(), c = counter_read() - boot_counter;
    uint64_t s = c / f, ns = (c % f) * 1000000000UL / f;
    if (clock == 0 || clock == 5 || clock == 8 || clock == 11)   /* the REALTIME and TAI clocks */
        s += boot_seconds;
    *sec = s, *nsec = ns;
}

/* Returns the clock's base, which is set on the first request.  The vDSO
 * keeps a copy of it (vm.c). */
void hal_clock_base(uint64_t *counter, uint64_t *seconds)
{
    if (!boot_counter) {
        boot_counter = counter_read();
        boot_seconds = rtc_seconds();
    }
    *counter = boot_counter, *seconds = boot_seconds;
}

void hal_now(uint64_t *sec, uint64_t *nsec)
{
    if (!boot_counter) {
        boot_counter = counter_read();
        boot_seconds = rtc_seconds();
    }
    now(0, sec, nsec);
}

static long sys_clock_gettime(long clock, uint64_t ts)
{
    uint64_t v[2];
    now((int)clock, &v[0], &v[1]);
    return copy_to_box(ts, v, 16) ? -EFAULT : 0;
}

/* nanosleep and clock_nanosleep: the thread waits while other threads run. */
static long sys_sleep(struct frame *fr, int clock, int flags, uint64_t req)
{
    uint64_t v[2];
    if (copy_from_box(v, req, 16))
        return -EFAULT;
    uint64_t f = counter_freq(), d = v[0] * f + v[1] * f / 1000000000UL, at;
    if (flags & 1) {                            /* TIMER_ABSTIME */
        uint64_t s0 = (clock == 0) ? boot_seconds : 0;
        at = boot_counter + (v[0] >= s0 ? (v[0] - s0) * f : 0) + v[1] * f / 1000000000UL;
    } else {
        at = counter_read() + d;
    }
    if ((int64_t)(counter_read() - at) >= 0)
        return 0;
    thread_block(fr, 0, at, 0);
    return SWITCHED;
}

static uint64_t rng = 0x2545F4914F6CDD1DUL;

static long sys_getrandom(uint64_t buf, uint64_t n)
{
    rng ^= counter_read();
    for (uint64_t i = 0; i < n; i++) {
        rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
        unsigned char b = (unsigned char)rng;
        if (copy_to_box(buf + i, &b, 1))
            return -EFAULT;
    }
    return (long)n;
}

static long sys_uname(uint64_t buf)
{
    static char u[6][65];
    memset(u, 0, sizeof u);
    memcpy(u[0], "BOX", 4);                     /* sysname */
    memcpy(u[1], "box", 4);                     /* nodename */
    memcpy(u[2], "HAL 0.01", 9);                /* release: as the banner shows it */
    memcpy(u[3], "HybrisOS 0.01", 14);     /* version */
    memcpy(u[4], "aarch64", 8);                 /* machine */
    return copy_to_box(buf, u, sizeof u) ? -EFAULT : 0;
}

static long sys_fstat(long fd, uint64_t st)
{
    if (!fd_ok(fd))
        return -EBADF;
    if (fds[fd].type == FD_FILE)
        return vfs_fstat(fds[fd].obj, st);
    uint32_t s[32];                             /* struct stat, which is 128 bytes */
    memset(s, 0, sizeof s);
    s[4] = fds[fd].type == FD_CONSOLE ? 0020620 : fds[fd].type == FD_INPUT_DIR ? 0040755
         : fds[fd].type == FD_INPUT || fds[fd].type == FD_PTM || fds[fd].type == FD_PTS ||
           fds[fd].type == FD_DEV ? 0020666 : fds[fd].type == FD_UNIX ? 0140777
         : fds[fd].type == FD_SOCKET ? 0140777
         : fds[fd].type == FD_PIPE_R || fds[fd].type == FD_PIPE_W ? 0010600 : 0100444;   /* st_mode */
    s[5] = 1;                                   /* st_nlink */
    if (fds[fd].type == FD_MEMFILE)
        *(uint64_t *)&s[12] = fds[fd].size;     /* st_size */
    *(uint32_t *)&s[14] = 4096;                 /* st_blksize */
    return copy_to_box(st, s, sizeof s) ? -EFAULT : 0;
}

/* The set-id calls.  A process whose effective uid is 0 may set any id.
 * Any other process may set only the ids it already has (real, effective or
 * saved).  So when sshd has dropped root and checks whether it can get root
 * back, it finds that it cannot, as it expects. */
static long sys_setid(uint64_t nr, const uint64_t *x)
{
    struct process *p = thread_current()->proc;
    int user = nr == 146 || nr == 145 || nr == 147;
    uint32_t *v = user ? p->uid : p->gid;
    int root = p->uid[1] == 0;
    uint32_t want[3] = { v[0], v[1], v[2] };
    if (nr == 146 || nr == 144) {               /* setuid, setgid */
        uint32_t id = (uint32_t)x[0];
        if (root)
            want[0] = want[1] = want[2] = id;
        else
            want[1] = id;
    } else {                                    /* setre*id (2 ids), setres*id (3) */
        /* An id of -1 leaves that id unchanged. */
        int n = nr == 145 || nr == 143 ? 2 : 3;
        for (int i = 0; i < n; i++)
            if ((uint32_t)x[i] != 0xFFFFFFFFu)
                want[i] = (uint32_t)x[i];
    }
    if (!root)
        for (int i = 0; i < 3; i++)
            if (want[i] != v[0] && want[i] != v[1] && want[i] != v[2])
                return -1;                      /* EPERM */
    memcpy(v, want, sizeof want);
    return 0;
}

/* The calls on an AF_UNIX socket (unix.c). */
static long sys_unix_call(struct frame *f, uint64_t nr, const uint64_t *x)
{
    int u = fds[x[0]].obj;
    switch (nr) {
    case 200: return unix_bind(u, x[1], x[2]);
    case 201: return unix_listen(u);
    case 202: case 242: {
        long o = unix_accept(f, u);
        if (o < 0 || o == SWITCHED)
            return o;
        int fd = fd_new(FD_UNIX);
        if (fd < 0) {
            unix_close((int)o);
            return fd;
        }
        fds[fd].obj = (int)o;
        if (nr == 242 && (x[3] & 04000))
            unix_nonblock((int)o, 1);
        if (x[1] && x[2])
            unix_name((int)o, x[1], x[2], 1);
        return fd;
    }
    case 203: return unix_connect(u, x[1], x[2]);
    case 204: return unix_name(u, x[1], x[2], 0);
    case 205: return unix_name(u, x[1], x[2], 1);
    case 206: return unix_send(f, u, x[1], x[2], (x[3] & 0x40) != 0);
    case 207: return unix_recv(f, u, x[1], x[2], (int)x[3]);
    case 208: return 0;                         /* setsockopt: accepted and ignored */
    case 209: return unix_getopt(u, (long)x[1], (long)x[2], x[3], x[4]);
    case 210: return unix_shutdown(u, (long)x[1]);
    case 211: case 212: {                       /* sendmsg, recvmsg: first iovec only */
        uint64_t m[7], v[2];                    /* musl's msghdr: name, namelen, iov, iovlen (int), ... */
        if (copy_from_box(m, x[1], sizeof m) || !(uint32_t)m[3] || copy_from_box(v, m[2], 16))
            return -EFAULT;
        long r = nr == 211 ? unix_send(f, u, v[0], v[1], (x[2] & 0x40) != 0) : unix_recv(f, u, v[0], v[1], (int)x[2]);
        if (nr == 212 && r >= 0) {
            uint32_t zero[2] = { 0, 0 };
            copy_to_box(x[1] + 40, zero, 4);    /* msg_controllen */
            copy_to_box(x[1] + 48, zero + 1, 4);    /* msg_flags */
        }
        return r;
    }
    }
    return -95;
}

static long sys_unix_ioctl(int u, uint32_t req, uint64_t arg)
{
    int v;
    if (req == 0x5421) {                        /* FIONBIO */
        if (copy_from_box(&v, arg, 4))
            return -EFAULT;
        unix_nonblock(u, v != 0);
        return 0;
    }
    if (req == 0x541B) {                        /* FIONREAD */
        v = (int)unix_fionread(u);
        return copy_to_box(arg, &v, 4) ? -EFAULT : 0;
    }
    return -ENOTTY;
}

/* FIONBIO on a pipe, an eventfd or the console. */
static long sys_fionbio(struct fd *f, uint64_t arg)
{
    int v;
    if (copy_from_box(&v, arg, 4))
        return -EFAULT;
    f->nonblock = v != 0;
    return 0;
}

/* The evdev ioctls that input_evdev.c uses: the device name and an axis's range. */
static long sys_evdev_ioctl(int dev, uint32_t req, uint64_t arg)
{
    uint32_t size = (req >> 16) & 0x3FFF, nr = req & 0xFF;
    if (((req >> 8) & 0xFF) != 'E')
        return -ENOTTY;
    if (nr == 0x06) {                           /* EVIOCGNAME(len) */
        const char *n = input_name(dev);
        uint32_t l = (uint32_t)strlen(n) + 1;
        if (l > size)
            l = size;
        return copy_to_box(arg, n, l) ? -EFAULT : (long)l;
    }
    if (nr == 0x40 || nr == 0x41) {             /* EVIOCGABS(ABS_X), (ABS_Y) */
        if (size < 24)
            return -EINVAL;
        return copy_to_box(arg, input_absinfo(dev, (int)nr - 0x40), 24) ? -EFAULT : 0;
    }
    return -EINVAL;
}

/* getdents64 on /dev/input: returns ".", "..", then one eventN for each device. */
static long sys_getdents(struct fd *f, uint64_t buf, uint64_t n)
{
    if (!f)
        return -EBADF;
    if (f->type != FD_INPUT_DIR)
        return -ENOTDIR;
    uint64_t done = 0;
    for (; f->pos < 2 + (uint64_t)input_devices(); f->pos++) {
        char name[16];
        if (f->pos < 2) {
            memcpy(name, "..", 3);
            name[f->pos + 1] = 0;
        } else {
            char *p = name;
            memcpy(p, "event", 5), p += 5;
            uint64_t i = f->pos - 2;
            if (i >= 10)
                *p++ = (char)('0' + i / 10);
            *p++ = (char)('0' + i % 10), *p = 0;
        }
        size_t nl = strlen(name);
        uint16_t reclen = (uint16_t)((19 + nl + 1 + 7) & ~7UL);
        if (done + reclen > n)
            break;
        unsigned char rec[48];
        memset(rec, 0, sizeof rec);
        *(uint64_t *)rec = f->pos + 1;          /* d_ino */
        *(int64_t *)(rec + 8) = (int64_t)f->pos + 1;    /* d_off */
        *(uint16_t *)(rec + 16) = reclen;
        rec[18] = f->pos < 2 ? 4 : 2;           /* DT_DIR, DT_CHR */
        memcpy(rec + 19, name, nl + 1);
        if (copy_to_box(buf + done, rec, reclen))
            return -EFAULT;
        done += reclen;
    }
    return (long)done;
}

/* The calls that take a path.  The path is in x[1], after a dirfd, except
 * for statfs and the attribute calls, where it is in x[0], and symlinkat,
 * where it is in x[2].  Symbolic links in the path are followed as the call
 * requires, and the share or the RAM file system (vfs.c) answers the call.
 * /proc/self/fd/N names an open file. */
static long sys_pathcall(uint64_t nr, long dirfd, const uint64_t *x)
{
    char path[768], to[768], canon[1024];
    uint64_t pva = nr == 43 || (nr >= 5 && nr <= 9) ? x[0] : nr == 36 ? x[2] : x[1];
    long e = box_path(nr == 36 ? (long)x[1] : dirfd, pva, path, sizeof path);
    if (e)
        return e;
    if (nr == 78 && !strncmp(path, "/proc/self/fd/", 14)) {        /* readlinkat */
        long fd = 0;
        for (const char *p = path + 14; *p >= '0' && *p <= '9'; p++)
            fd = fd * 10 + (*p - '0');
        if (!fd_ok(fd) || fds[fd].type != FD_FILE)
            return -EINVAL;
        const char *sp = vfs_path(fds[fd].obj);
        uint64_t l = strlen(sp);
        if (l > x[3])
            l = x[3];
        return copy_to_box(x[2], sp, l) ? -EFAULT : (long)l;
    }
    int nofollow = (nr == 79 || nr == 88) && (x[3] & 0x100);      /* AT_SYMLINK_NOFOLLOW */
    int follow = !(nr == 34 || nr == 35 || nr == 38 || nr == 276 || nr == 78 || nr == 36 ||
                   nr == 6 || nr == 9 || nofollow);
    if ((e = vfs_resolve(path, canon, follow)))
        return e;
    static const char *const devs[] = { "/dev/null", "/dev/zero", "/dev/urandom", "/dev/random",
                                        "/dev/full", "/dev/tty", "/dev/console", "/dev/ptmx" };
    for (int i = 0; i < 8; i++)
        if (!strcmp(canon, devs[i])) {          /* the HAL's devices exist, as character devices */
            if (nr == 48 || nr == 439)
                return 0;
            if (nr == 79) {
                uint64_t st[16];
                memset(st, 0, sizeof st);
                st[0] = 5, st[1] = 100 + (uint64_t)i;
                st[2] = 0020666 | 1UL << 32;
                st[7] = 4096;
                return copy_to_box(x[2], st, 128) ? -EFAULT : 0;
            }
            return nr == 78 ? -EINVAL : -13;
        }
    const char *text = hal_text(canon);
    if (text) {                                 /* a HAL file: exists, read-only */
        if (nr == 48 || nr == 439)
            return x[2] & 2 ? -13 : 0;          /* W_OK gives EACCES */
        if (nr == 79) {
            uint64_t st[16];
            memset(st, 0, sizeof st);
            st[0] = 3, st[1] = 1;
            st[2] = 0100444 | 1UL << 32;
            st[6] = strlen(text), st[7] = 4096;
            return copy_to_box(x[2], st, 128) ? -EFAULT : 0;
        }
        return nr == 78 ? -EINVAL : -13;
    }
    char name[256];
    if (nr >= 5 && nr <= 9) {                   /* read the attribute's name */
        long n = strlen_box(x[1], sizeof name - 1);
        if (n < 0)
            return -EFAULT;
        copy_from_box(name, x[1], (size_t)n + 1);
    }
    switch (nr) {
    case 43: return vfs_statfs(canon, x[1]);
    case 79: return vfs_stat(canon, x[2], follow);
    case 53: return vfs_chmod(canon, (uint32_t)x[2]);
    case 34: return vfs_mkdir(canon, (uint32_t)x[2] & ~thread_current()->proc->umask);
    case 35: return vfs_unlink(canon, (long)x[2]);
    case 38: case 276: {                        /* renameat, renameat2 */
        if (nr == 276 && x[4])
            return -EINVAL;                     /* flags are not supported */
        char tc[1024];
        if ((e = box_path((long)x[2], x[3], to, sizeof to)) || (e = vfs_resolve(to, tc, 0)))
            return e;
        return vfs_rename(canon, tc);
    }
    case 36: {                                  /* symlinkat: the target is kept as given */
        long n = strlen_box(x[0], sizeof to - 1);
        if (n < 0)
            return -EFAULT;
        copy_from_box(to, x[0], (size_t)n + 1);
        return vfs_symlink(to, canon);
    }
    case 88: return vfs_utimens(canon, x[2], follow);
    case 48: case 439: return vfs_access(canon);
    case 78: return vfs_readlink(canon, x[2], x[3]);
    case 8: case 9: return vfs_getxattr(canon, name, x[2], x[3], follow);
    case 5: case 6: return vfs_setxattr(canon, name, x[2], x[3], (long)x[4], follow);
    }
    return -ENOSYS;
}

static const char *const names[300] = {
    [17] = "getcwd", [19] = "eventfd2", [36] = "symlinkat", [43] = "statfs", [53] = "fchmodat", [67] = "pread64", [20] = "epoll_create1", [23] = "dup", [24] = "dup3",
    [25] = "fcntl", [26] = "inotify_init1", [29] = "ioctl", [34] = "mkdirat", [35] = "unlinkat",
    [38] = "renameat", [39] = "umount2", [40] = "mount", [46] = "ftruncate", [48] = "faccessat",
    [49] = "chdir", [56] = "openat", [57] = "close", [59] = "pipe2", [61] = "getdents64",
    [62] = "lseek", [63] = "read", [64] = "write", [65] = "readv", [66] = "writev", [73] = "ppoll",
    [78] = "readlinkat", [79] = "newfstatat", [80] = "fstat", [93] = "exit", [94] = "exit_group",
    [96] = "set_tid_address", [98] = "futex", [99] = "set_robust_list", [101] = "nanosleep",
    [113] = "clock_gettime", [115] = "clock_nanosleep", [123] = "sched_getaffinity",
    [124] = "sched_yield", [129] = "kill", [130] = "tkill", [131] = "tgkill", [132] = "sigaltstack",
    [134] = "rt_sigaction", [135] = "rt_sigprocmask", [139] = "rt_sigreturn", [142] = "reboot",
    [160] = "uname", [163] = "getrlimit", [169] = "gettimeofday", [172] = "getpid",
    [178] = "gettid", [166] = "umask", [51] = "chroot", [103] = "setitimer", [159] = "setgroups", [47] = "fallocate", [137] = "rt_sigtimedwait", [260] = "wait4", [81] = "sync", [232] = "mincore", [179] = "sysinfo", [198] = "socket", [199] = "socketpair", [200] = "bind", [201] = "listen",
    [202] = "accept", [203] = "connect", [204] = "getsockname", [205] = "getpeername", [206] = "sendto",
    [207] = "recvfrom", [208] = "setsockopt", [209] = "getsockopt", [210] = "shutdown", [211] = "sendmsg",
    [212] = "recvmsg", [242] = "accept4", [214] = "brk", [215] = "munmap",
    [216] = "mremap", [220] = "clone", [221] = "execve", [222] = "mmap", [226] = "mprotect",
    [233] = "madvise", [261] = "prlimit64", [278] = "getrandom", [279] = "memfd_create",
};
static unsigned char reported[300];

/* Makes the console /init's standard input, output and error, as Linux
 * gives /dev/console to process 1. */
void syscall_init(void)
{
    for (int i = 0; i < 3; i++)
        fds[i].type = FD_CONSOLE;
}

void syscall(struct frame *f)
{
    if (!boot_counter) {
        boot_counter = counter_read();
        boot_seconds = rtc_seconds();
    }
    uint64_t *x = f->x, nr = x[8];
    thread_current()->last_nr = nr;
    thread_current()->recall = 0;               /* the thread has made its call again */
    counts.syscalls++;
    long r;
    switch (nr) {
    case 63: r = sys_read(f, (long)x[0], x[1], x[2]); break;
    case 64: r = sys_write(f, (long)x[0], x[1], x[2]); break;
    case 73: r = sys_ppoll(f, x[0], x[1], x[2], x[3]); break;
    case 65: r = sys_readv(f, (long)x[0], x[1], (long)x[2]); break;
    case 67:                                    /* pread64 */
    case 68:                                    /* pwrite64 */
        if (fd_ok((long)x[0]) && fds[x[0]].type == FD_FILE) {
            r = (nr == 67 ? vfs_read : vfs_write)(fds[x[0]].obj, x[1], x[2], (int64_t)x[3] < 0 ? 0 : (int64_t)x[3]);
            break;
        }
        if (nr == 68) {
            r = fd_ok((long)x[0]) ? -ESPIPE : -EBADF;
            break;
        }
        {
        if (!fd_ok((long)x[0]) || fds[x[0]].type != FD_MEMFILE) {
            r = fd_ok((long)x[0]) ? -ESPIPE : -EBADF;
            break;
        }
        struct fd *m = &fds[x[0]];
        uint64_t off = x[3], n = x[2];
        if (off > m->size)
            off = m->size;
        if (n > m->size - off)
            n = m->size - off;
        r = copy_to_box(x[1], m->data + off, n) ? -EFAULT : (long)n;
        break;
    }
    case 43: r = sys_pathcall(nr, AT_FDCWD, x); break;     /* statfs */
    case 44:                                    /* fstatfs */
        r = fd_ok((long)x[0]) && fds[x[0]].type == FD_FILE ? vfs_statfs(vfs_path(fds[x[0]].obj), x[1])
            : fd_ok((long)x[0]) ? -ENOSYS : -EBADF;
        break;
    case 82: case 83:                           /* fsync, fdatasync */
        r = fd_ok((long)x[0]) && fds[x[0]].type == FD_FILE ? vfs_fsync(fds[x[0]].obj)
            : fd_ok((long)x[0]) ? 0 : -EBADF;
        break;
    case 53: case 34: case 35: case 38: case 276: case 88: case 48: case 439: case 78: case 36:
    case 5: case 6: case 8: case 9:             /* fchmodat to lgetxattr: calls by path */
        r = sys_pathcall(nr, (long)x[0], x);
        break;
    case 7: case 10:                            /* fsetxattr, fgetxattr */
        if (!fd_ok((long)x[0]) || fds[x[0]].type != FD_FILE) {
            r = fd_ok((long)x[0]) ? -95 : -EBADF;       /* EOPNOTSUPP */
            break;
        }
        {
            char name[256];
            long n = strlen_box(x[1], sizeof name - 1);
            if (n < 0) {
                r = -EFAULT;
                break;
            }
            copy_from_box(name, x[1], (size_t)n + 1);
            const char *p = vfs_path(fds[x[0]].obj);
            r = nr == 10 ? vfs_getxattr(p, name, x[2], x[3], 1) : vfs_setxattr(p, name, x[2], x[3], (long)x[4], 1);
        }
        break;
    /* inotify_init1 is not provided; the caller manages without it. */
    case 26: r = -ENOSYS; break;
    case 168: {                                 /* getcpu: this core, and node 0 */
        uint32_t c = (uint32_t)this_cpu()->id, z = 0;
        r = (x[0] && copy_to_box(x[0], &c, 4)) || (x[1] && copy_to_box(x[1], &z, 4)) ? -EFAULT : 0;
        break;
    }
    /* sync: the share writes through, and everything else is in memory. */
    case 81: r = 0; break;
    case 166:                                   /* umask: per process, used at creation */
        r = thread_current()->proc->umask;
        thread_current()->proc->umask = (unsigned)x[0] & 0777;
        break;
    case 159: r = thread_current()->proc->uid[1] ? -1 : 0; break;     /* setgroups: root only */
    case 158: r = 0; break;                     /* getgroups: none */
    case 47: r = -95; break;                    /* fallocate: EOPNOTSUPP; caller writes instead */
    case 137: {                                 /* rt_sigtimedwait: takes a signal in the set */
        uint64_t set;
        if (copy_from_box(&set, x[0], 8)) {
            r = -EFAULT;
            break;
        }
        struct thread *t = thread_current();
        uint64_t ready = t->pending & set;
        if (ready) {
            int sig = __builtin_ctzl(ready) + 1;
            t->pending &= ~(1UL << (sig - 1));
            if (x[1]) {
                int32_t si[32];
                memset(si, 0, sizeof si);
                si[0] = sig;
                copy_to_box(x[1], si, sizeof si);
            }
            r = sig;
        } else {
            r = -EAGAIN;                        /* does not wait: none of the box's callers wait */
        }
        break;
    }
    case 232: r = vm_mincore(x[0], x[1], x[2]); break;
    case HAL_SYS_AUDIO: {                       /* HAL call: a descriptor for writing sound */
        if (!sound_present()) {
            r = -19;                            /* ENODEV */
            break;
        }
        int fd = fd_new(FD_AUDIO);
        uint32_t info[2] = { (uint32_t)fd, 44100 };
        r = fd < 0 ? fd : copy_to_box(x[0], info, 8) ? -EFAULT : 0;
        break;
    }
    case HAL_SYS_FLUSH: r = gpu_flush((uint32_t)x[0], (uint32_t)x[1], (uint32_t)x[2], (uint32_t)x[3]); break;
    case HAL_SYS_STATS: r = stats_read(x[0], x[1]); break;
    /* HAL call: the screen as a memfd; x1 and x2 give the size wanted. */
    case HAL_SYS_SCREEN: {
        struct hal_screen sc;
        int obj = gpu_present() ? gpu_screen((uint32_t)x[1], (uint32_t)x[2], &sc) : ramfb_screen(&sc);
        if (obj >= 0)
            sc.gpu = gpu_present();
        if (obj < 0) {
            r = obj;
            break;
        }
        int fd = fd_new(FD_MEMFD);
        if (fd < 0) {
            r = fd;
            break;
        }
        fds[fd].obj = obj;
        sc.fd = fd;
        r = copy_to_box(x[0], &sc, sizeof sc) ? -EFAULT : 0;
        break;
    }
    case 59: {                                  /* pipe2 */
        int p = -1;
        for (int i = 0; i < PIPES; i++)
            if (!pipes[i].used) {
                p = i;
                break;
            }
        int rd = p < 0 ? -EMFILE : fd_new(FD_PIPE_R);
        int wr = rd < 0 ? rd : fd_new(FD_PIPE_W);
        if (rd < 0 || wr < 0) {
            if (rd >= 0)
                fds[rd].type = FD_FREE;
            r = -EMFILE;
            break;
        }
        memset(&pipes[p], 0, sizeof pipes[p]);
        pipes[p].used = 1, pipes[p].readers = 1, pipes[p].writers = 1;
        fds[rd].obj = fds[wr].obj = p;
        fds[rd].nonblock = fds[wr].nonblock = (x[1] & 04000) != 0;     /* O_NONBLOCK */
        fds[rd].cloexec = fds[wr].cloexec = (x[1] & 02000000) != 0;    /* O_CLOEXEC */
        int32_t two[2] = {rd, wr};
        r = copy_to_box(x[0], two, 8) ? -EFAULT : 0;
        break;
    }
    case 19: {                                  /* eventfd2 */
        int e = -1;
        for (int i = 0; i < PIPES; i++)
            if (!eventfds[i].used) {
                e = i;
                break;
            }
        int fd = e < 0 ? -EMFILE : fd_new(FD_EVENTFD);
        if (fd < 0) {
            r = -EMFILE;
            break;
        }
        eventfds[e].used = 1, eventfds[e].refs = 1, eventfds[e].value = x[0];
        fds[fd].obj = e;
        fds[fd].nonblock = (x[1] & 04000) != 0;                         /* EFD_NONBLOCK */
        r = fd;
        break;
    }
    case 66: r = sys_writev((long)x[0], x[1], (long)x[2]); break;
    case 56: r = sys_openat((long)x[0], x[1], (long)x[2], (long)x[3] & ~(long)thread_current()->proc->umask); break;
    case 57:                                    /* close */
        if (!fd_ok((long)x[0]))
            r = -EBADF;
        else
            release((int)x[0]), r = 0;
        break;
    case 25:                                    /* fcntl: socket O_NONBLOCK; others accepted */
        if (!fd_ok((long)x[0])) {
            r = -EBADF;
        } else if (fds[x[0]].type == FD_UNIX && (x[1] == 3 || x[1] == 4)) {
            if (x[1] == 4)
                unix_nonblock(fds[x[0]].obj, (x[2] & 04000) != 0);
            r = x[1] == 3 ? 2 | (unix_nonblock(fds[x[0]].obj, -1) ? 04000 : 0) : 0;
        } else if (fds[x[0]].type == FD_SOCKET && x[1] == 3) {        /* F_GETFL */
            r = 2 | (sock_nonblock(fds[x[0]].obj, -1) ? 04000 : 0);
        } else if (fds[x[0]].type == FD_SOCKET && x[1] == 4) {        /* F_SETFL */
            sock_nonblock(fds[x[0]].obj, (x[2] & 04000) != 0);
            r = 0;
        } else if (x[1] == 3) {
            r = (fds[x[0]].type == FD_FILE || fds[x[0]].type == FD_CONSOLE ? 2 : 0) |
                (fds[x[0]].nonblock ? 04000 : 0);
        } else if (x[1] == 4) {
            fds[x[0]].nonblock = (x[2] & 04000) != 0;
            r = 0;
        } else if (x[1] == 1) {                 /* F_GETFD */
            r = fds[x[0]].cloexec ? 1 : 0;
        } else if (x[1] == 2) {                 /* F_SETFD */
            fds[x[0]].cloexec = x[2] & 1;
            r = 0;
        } else if (x[1] == 0 || x[1] == 1030) { /* F_DUPFD(_CLOEXEC): lowest free from x2 */
            long to = (long)x[2];
            while (to < FDS && fds[to].type != FD_FREE)
                to++;
            if (to >= FDS) {
                r = -EMFILE;
                break;
            }
            fds[to] = fds[x[0]];
            hold((int)to);
            fds[to].cloexec = x[1] == 1030;
            r = to;
        } else {
            r = 0;
        }
        break;
    case 29:                                    /* ioctl: evdev's on an input device, and more */
        r = !fd_ok((long)x[0]) ? -EBADF
            : fds[x[0]].type == FD_INPUT ? sys_evdev_ioctl(fds[x[0]].obj, (uint32_t)x[1], x[2])
            : fds[x[0]].type == FD_SOCKET ? sock_ioctl(fds[x[0]].obj, (uint32_t)x[1], x[2])
            : fds[x[0]].type == FD_UNIX ? sys_unix_ioctl(fds[x[0]].obj, (uint32_t)x[1], x[2])
            : fds[x[0]].type == FD_AUDIO ? sound_ioctl((uint32_t)x[1])
            : fds[x[0]].type == FD_PTM || fds[x[0]].type == FD_PTS
              ? (x[1] == 0x5421 ? sys_fionbio(&fds[x[0]], x[2]) : tty_ioctl(fds[x[0]].obj, fds[x[0]].type == FD_PTM, (uint32_t)x[1], x[2]))
            : x[1] == 0x5421 ? sys_fionbio(&fds[x[0]], x[2]) : -ENOTTY;
        break;
    case 61:
        r = fd_ok((long)x[0]) && fds[x[0]].type == FD_FILE ? vfs_getdents(fds[x[0]].obj, x[1], x[2])
            : sys_getdents(fd_ok((long)x[0]) ? &fds[x[0]] : 0, x[1], x[2]);
        break;
    case 23:                                    /* dup */
    case 24: {                                  /* dup3 */
        long from = (long)x[0];
        if (!fd_ok(from)) {
            r = -EBADF;
            break;
        }
        long to = nr == 24 ? (long)x[1] : fd_new(FD_CONSOLE);
        if (to < 0 || to >= FDS) {
            r = -EBADF;
            break;
        }
        if (fd_ok(to) && to != from)
            release((int)to);
        fds[to] = fds[from];
        hold((int)to);
        r = to;
        break;
    }
    case 62:                                    /* lseek */
        r = fd_ok((long)x[0]) && fds[x[0]].type == FD_FILE ? vfs_seek(fds[x[0]].obj, (int64_t)x[1], (int)x[2])
            : fd_ok((long)x[0]) ? -ESPIPE : -EBADF;
        break;
    case 79:                                    /* newfstatat */
        if ((long)x[0] >= 0 && x[3] & 0x1000) { /* AT_EMPTY_PATH */
            uint8_t c = 0;
            copy_from_box(&c, x[1], 1);
            if (!c) {
                r = sys_fstat((long)x[0], x[2]);
                break;
            }
        }
        r = sys_pathcall(nr, (long)x[0], x);
        break;
    case 80: r = sys_fstat((long)x[0], x[1]); break;
    case 17: {                                  /* getcwd: the process's current directory */
        const char *cwd = thread_current()->proc->cwd;
        uint64_t l = strlen(cwd) + 1;
        r = x[1] < l ? -34 : copy_to_box(x[0], cwd, l) ? -EFAULT : (long)l;  /* ERANGE */
        break;
    }
    case 49: case 50: {                         /* chdir, fchdir: per process */
        char path[768], canon[1024];
        long e = 0;
        if (nr == 50) {
            if (!fd_ok((long)x[0]) || fds[x[0]].type != FD_FILE) {
                r = -EBADF;
                break;
            }
            memcpy(path, vfs_path(fds[x[0]].obj), strlen(vfs_path(fds[x[0]].obj)) + 1);
        } else {
            e = box_path(AT_FDCWD, x[0], path, sizeof path);
        }
        if (!e)
            e = vfs_resolve(path, canon, 1);
        if (!e && strlen(canon) >= sizeof thread_current()->proc->cwd)
            e = -36;                            /* ENAMETOOLONG */
        if (!e)
            e = vfs_access(canon);              /* checks it exists; a file passes as a directory */
        if (!e)
            memcpy(thread_current()->proc->cwd, canon, strlen(canon) + 1);
        r = e;
        break;
    }
    case 51: {                                  /* chroot: sshd's privilege separation uses it
                                                   to enter an empty directory.  The HAL checks
                                                   that the directory exists and reports success
                                                   without changing the root, because the box has
                                                   one user. */
        char path[768], canon[1024];
        long e = box_path(AT_FDCWD, x[0], path, sizeof path);
        if (!e)
            e = vfs_resolve(path, canon, 1);
        r = e ? e : vfs_access(canon);
        break;
    }
    case 103: case 102: {                       /* setitimer, getitimer: ITIMER_REAL */
        if (x[0] != 0) {
            r = -EINVAL;
            break;
        }
        struct process *pr = thread_current()->proc;
        uint64_t fq = counter_freq(), now = counter_read();
        uint64_t left = pr->alarm_at && (int64_t)(pr->alarm_at - now) > 0 ? pr->alarm_at - now : 0;
        int64_t old[4] = { (int64_t)(pr->alarm_every / fq), (int64_t)(pr->alarm_every % fq * 1000000 / fq),
                           (int64_t)(left / fq), (int64_t)(left % fq * 1000000 / fq) };
        uint64_t oldp = nr == 103 ? x[2] : x[1];
        if (nr == 103 && x[1]) {
            int64_t nv[4];
            if (copy_from_box(nv, x[1], 32)) {
                r = -EFAULT;
                break;
            }
            pr->alarm_every = (uint64_t)nv[0] * fq + (uint64_t)nv[1] * fq / 1000000;
            uint64_t first = (uint64_t)nv[2] * fq + (uint64_t)nv[3] * fq / 1000000;
            pr->alarm_at = first ? now + first : 0;
        }
        r = oldp && copy_to_box(oldp, old, 32) ? -EFAULT : 0;
        break;
    }
    case 40: {                                  /* mount: only the share, as 9p */
        char type[16] = "";
        long n = x[2] ? strlen_box(x[2], sizeof type - 1) : -1;
        if (n >= 0)
            copy_from_box(type, x[2], (size_t)n + 1);
        r = !strcmp(type, "9p") && share_has("/host") ? 0 : -ENODEV;
        break;
    }

    case 172: r = thread_current()->proc->pid; break;      /* getpid */
    case 173: r = thread_current()->proc->ppid; break;     /* getppid */
    case 155: case 156: r = (long)x[0] ? (long)x[0] : thread_current()->proc->pid; break;  /* getpgid, getsid */
    case 260: r = proc_wait(f, (long)(int)x[0], x[1], (long)x[2]); break;  /* wait4 */
    case 221: r = proc_execve(f, x[0], x[1], x[2]); break;                 /* execve */
    case 178: r = thread_current()->tid; break; /* gettid */
    case 96:                                    /* set_tid_address */
        thread_current()->clear_tid = x[0];
        r = thread_current()->tid;
        break;
    case 99: r = 0; break;                      /* set_robust_list */
    case 174: r = thread_current()->proc->uid[0]; break;   /* getuid */
    case 175: r = thread_current()->proc->uid[1]; break;   /* geteuid */
    case 176: r = thread_current()->proc->gid[0]; break;   /* getgid */
    case 177: r = thread_current()->proc->gid[1]; break;   /* getegid */
    case 146: case 144: case 145: case 143: case 147: case 149:
        r = sys_setid(nr, x);                   /* setuid to setresgid, by Linux's rules */
        break;
    case 151: case 152: r = 0; break;           /* setfsuid, setfsgid: return the old id, 0 */
    case 148: case 150: {                       /* getresuid, getresgid */
        uint32_t *v = nr == 148 ? thread_current()->proc->uid : thread_current()->proc->gid;
        r = copy_to_box(x[0], &v[0], 4) || copy_to_box(x[1], &v[1], 4) || copy_to_box(x[2], &v[2], 4) ? -EFAULT : 0;
        break;
    }
    case 157: case 154: case 167: r = 0; break; /* setsid, setpgid, prctl */
    case 160: r = sys_uname(x[0]); break;

    case 134: r = signal_action((long)x[0], x[1], x[2]); break;   /* rt_sigaction */
    case 135: r = signal_procmask((long)x[0], x[1], x[2]); break; /* rt_sigprocmask */
    case 132: r = signal_altstack(x[0], x[1]); break;             /* sigaltstack */
    case 139:                                   /* rt_sigreturn: the whole frame, x0 too */
        signal_return(f);
        return;
    case 129:                                   /* kill: to another process */
        if ((long)(int)x[0] > 0 && (int)x[0] != thread_current()->proc->pid) {
            r = proc_kill((long)(int)x[0], (long)x[1]);
            break;
        }
        __attribute__((fallthrough));           /* to the caller's own process */
    case 130: case 131: {                       /* tkill, tgkill: to a thread */
        long sig = (long)(nr == 131 ? x[2] : x[1]);
        long tid = (long)(nr == 131 ? x[1] : nr == 130 ? x[0] : 0);
        x[0] = 0;
        if (!sig)
            return;
        struct thread *to = tid ? thread_by_tid((int)tid) : 0;
        if (tid && !to) {
            x[0] = (uint64_t)-3;                /* ESRCH */
            return;
        }
        if (to && to != thread_current()) {     /* another thread: delivered when it runs */
            to->pending |= 1UL << (sig - 1);
            thread_interrupt(to);
            return;
        }
        if (signal_deliver(f, (int)sig, -6, 0) < 0) {      /* SI_TKILL */
            kprintf("HAL: /init sent itself signal %ld, which it does not handle\n", sig);
            psci_off();
        }
        return;
    }

    case 214: r = vm_brk(x[0]); break;
    case 222: r = vm_mmap(x[0], x[1], (int)x[2], (int)x[3], (int)x[4], x[5]); break;
    case 215: r = vm_munmap(x[0], x[1]); break;
    case 226: r = vm_mprotect(x[0], x[1], (int)x[2]); break;
    case 233: r = 0; break;                     /* madvise */
    case 216: r = -ENOMEM; break;               /* mremap: refused, so musl copies instead */
    case 279: {                                 /* memfd_create */
        int obj = memfd_obj_new();
        if (obj < 0) {
            r = obj;
            break;
        }
        int fd = fd_new(FD_MEMFD);
        if (fd >= 0)
            fds[fd].obj = obj;
        r = fd;
        break;
    }
    case 46:                                    /* ftruncate */
        if (fd_ok((long)x[0]) && fds[x[0]].type == FD_FILE)
            r = vfs_truncate(fds[x[0]].obj, x[1]);
        else
            r = memfd_obj_of_fd((int)x[0]) < 0 ? -EINVAL : memfd_obj_truncate(memfd_obj_of_fd((int)x[0]), x[1]);
        break;

    case 113: r = sys_clock_gettime((long)x[0], x[1]); break;
    case 114: {                                 /* clock_getres: 1 ns */
        uint64_t v[2] = {0, 1};
        r = x[1] && copy_to_box(x[1], v, 16) ? -EFAULT : 0;
        break;
    }
    case 169: {                                 /* gettimeofday */
        uint64_t v[2];
        now(0, &v[0], &v[1]);
        v[1] /= 1000;
        r = x[0] && copy_to_box(x[0], v, 16) ? -EFAULT : 0;
        break;
    }
    case 101: r = sys_sleep(f, 1, 0, x[0]); break;                 /* nanosleep */
    case 115: r = sys_sleep(f, (int)x[0], (int)x[1], x[2]); break; /* clock_nanosleep */
    case 124:                                   /* sched_yield */
        x[0] = 0;
        schedule(f);
        return;
    case 123: {                                 /* sched_getaffinity: allowed cores */
        struct thread *t = x[0] ? thread_by_tid((int)x[0]) : thread_current();
        uint64_t all = (1UL << smp_count()) - 1, mask = t && t->cpus ? t->cpus & all : all;
        r = !t ? -ESRCH : x[1] < 8 ? -EINVAL : copy_to_box(x[2], &mask, 8) ? -EFAULT : 8;
        break;
    }
    case 122: {                                 /* sched_setaffinity: the cores a thread may run on */
        struct thread *t = x[0] ? thread_by_tid((int)x[0]) : thread_current();
        uint64_t mask = 0, all = (1UL << smp_count()) - 1;
        if (!t)
            r = -ESRCH;
        else if (copy_from_box(&mask, x[2], x[1] < 8 ? x[1] : 8))
            r = -EFAULT;
        else if (!(mask & all))
            r = -EINVAL;
        else {
            t->cpus = (mask & all) == all ? 0 : (uint32_t)(mask & all);
            if (t == thread_current() && t->cpus && !(t->cpus >> this_cpu()->id & 1)) {
                x[0] = 0;                       /* move to a core it may use */
                schedule(f);
                return;
            }
            r = 0;
        }
        break;
    }
    case 119: r = 0; break;                     /* sched_setscheduler */
    case 278: r = sys_getrandom(x[0], x[1]); break;
    case 163: case 261: {                       /* getrlimit, prlimit64: Linux's for process 1 */
        uint64_t res = nr == 163 ? x[0] : x[1];
        uint64_t lim[2] = {~0UL, ~0UL};
        if (res == 3)                           /* RLIMIT_STACK: main thread (load.c) */
            lim[0] = 8UL << 20;
        else if (res == 7)                      /* RLIMIT_NOFILE: the table size */
            lim[0] = lim[1] = FDS;
        uint64_t where = nr == 163 ? x[1] : x[3];
        r = where && copy_to_box(where, lim, 16) ? -EFAULT : 0;
        break;
    }
    case 179: {                                 /* sysinfo */
        uint64_t si[14];
        memset(si, 0, sizeof si);
        si[4] = boot.ram_size;                  /* totalram */
        si[5] = pages_free() * PAGE_SIZE;       /* freeram */
        ((uint32_t *)si)[26] = 1;               /* mem_unit */
        r = copy_to_box(x[0], si, sizeof si) ? -EFAULT : 0;
        break;
    }
    case 98: {                                  /* futex */
        unsigned op = (unsigned)x[1] & 127;
        uint64_t uaddr = x[0];
        if (op == 0 || op == 9) {               /* WAIT, WAIT_BITSET */
            uint32_t v;
            if (copy_from_box(&v, uaddr, 4)) {
                r = -EFAULT;
                break;
            }
            if (v != (uint32_t)x[2]) {
                r = -EAGAIN;
                break;
            }
            uint64_t at = 0;
            if (x[3]) {
                uint64_t ts[2];
                if (copy_from_box(ts, x[3], 16)) {
                    r = -EFAULT;
                    break;
                }
                uint64_t fq = counter_freq(), d = ts[0] * fq + ts[1] * fq / 1000000000UL;
                if (op == 9) {                  /* an absolute time, on the requested clock */
                    uint64_t s0 = ((unsigned)x[1] & 256) ? boot_seconds : 0;
                    at = boot_counter + (ts[0] >= s0 ? (ts[0] - s0) * fq : 0) + ts[1] * fq / 1000000000UL;
                } else {
                    at = counter_read() + d;
                }
                if (!at)
                    at = 1;
            }
            thread_block(f, uaddr, at, -ETIMEDOUT);
            return;
        }
        if (op == 1 || op == 10) {              /* WAKE, WAKE_BITSET */
            r = futex_wake(uaddr, (long)(int)x[2]);
            break;
        }
        if (op == 3 || op == 4) {               /* REQUEUE, CMP_REQUEUE */
            if (op == 4) {
                uint32_t v;
                if (copy_from_box(&v, uaddr, 4)) {
                    r = -EFAULT;
                    break;
                }
                if (v != (uint32_t)x[5]) {
                    r = -EAGAIN;
                    break;
                }
            }
            r = futex_requeue(uaddr, (long)(int)x[2], (long)x[3], x[4]);
            break;
        }
        r = -ENOSYS;
        break;
    }
    case 198: {                                 /* socket */
        if (x[0] == 1) {                        /* AF_UNIX */
            long o = unix_socket((long)x[1]);
            int fd = o < 0 ? (int)o : fd_new(FD_UNIX);
            if (o >= 0 && fd < 0)
                unix_close((int)o);
            if (fd >= 0)
                fds[fd].obj = (int)o;
            r = o < 0 ? o : fd;
            break;
        }
        long o = sock_socket((long)x[0], (long)x[1], (long)x[2]);
        r = o < 0 ? o : new_socket_fd(o);
        break;
    }
    case 199: {                                 /* socketpair: AF_UNIX stream sockets only */
        int pair[2], fa, fb;
        if (x[0] != 1) {
            r = -95;
            break;
        }
        if ((r = unix_pair((long)x[1], pair)))
            break;
        fa = fd_new(FD_UNIX);
        fb = fa < 0 ? fa : fd_new(FD_UNIX);
        if (fa < 0 || fb < 0) {
            if (fa >= 0)
                fds[fa].type = FD_FREE;
            unix_close(pair[0]), unix_close(pair[1]);
            r = -EMFILE;
            break;
        }
        fds[fa].obj = pair[0], fds[fb].obj = pair[1];
        fds[fa].cloexec = fds[fb].cloexec = (x[1] & 02000000) != 0;
        int32_t two[2] = { fa, fb };
        r = copy_to_box(x[3], two, 8) ? -EFAULT : 0;
        break;
    }
    case 200: case 201: case 202: case 242: case 203: case 204: case 205: case 206: case 207:
    case 208: case 209: case 210: case 211: case 212: {
        if (fd_ok((long)x[0]) && fds[x[0]].type == FD_UNIX) {
            r = sys_unix_call(f, nr, x);
            break;
        }
        if (!fd_ok((long)x[0]) || fds[x[0]].type != FD_SOCKET) {
            r = fd_ok((long)x[0]) ? -88 : -EBADF;       /* ENOTSOCK */
            break;
        }
        int so = fds[x[0]].obj;
        switch (nr) {
        case 200: r = sock_bind(so, x[1], x[2]); break;
        case 201: r = sock_listen(so, (long)x[1]); break;
        case 202: case 242: {                   /* accept, accept4 */
            long o = sock_accept(f, so, x[1], x[2]);
            if (o >= 0) {
                if (nr == 242 && (x[3] & 04000))
                    sock_nonblock((int)o, 1);
                o = new_socket_fd(o);
            }
            r = o;
            break;
        }
        case 203: r = sock_connect(f, so, x[1], x[2]); break;
        case 204: r = sock_name(so, x[1], x[2], 0); break;
        case 205: r = sock_name(so, x[1], x[2], 1); break;
        case 206: r = sock_send(f, so, x[1], x[2], (long)x[3], x[4], x[5]); break;
        case 207: r = sock_recv(f, so, x[1], x[2], (long)x[3], x[4], x[5]); break;
        case 208: r = sock_setopt(so, (long)x[1], (long)x[2], x[3], x[4]); break;
        case 209: r = sock_getopt(so, (long)x[1], (long)x[2], x[3], x[4]); break;
        case 210: r = sock_shutdown(so, (long)x[1]); break;
        case 211: r = sock_sendmsg(f, so, x[1], (long)x[2]); break;
        default: r = sock_recvmsg(f, so, x[1], (long)x[2]); break;
        }
        break;
    }
    case 220: r = thread_clone(f); break;       /* clone: makes threads */

    case 93:                                    /* exit: ends this thread */
        thread_exit(f);
        return;
    case 94:                                    /* exit_group: the process; for /init, the box */
        proc_exit(thread_current()->proc, (int)((x[0] & 0xFF) << 8));
        thread_end_quiet(f);
        return;
    case 142:                                   /* reboot */
        if (x[2] == 0x01234567)
            psci_reset();
        kprintf("HAL: /init asked to power off\n");
        psci_off();

    default:
        if (nr < 300 && !reported[nr]) {
            reported[nr] = 1;
            kprintf("HAL: system call %lu (%s) not answered yet\n", nr, names[nr] ? names[nr] : "?");
        }
        r = -ENOSYS;
    }
    /* If hal.netlog is on the command line, log every socket call and its result. */
    static int netlog = -1;
    if (netlog < 0)
        netlog = strstr(boot.bootargs, "hal.netlog") != 0;
    if (netlog && ((nr >= 198 && nr <= 212) || nr == 242) && r != SWITCHED)
        kprintf("HAL: tid %d %s(%ld, %lx, %lx) -> %ld\n", thread_current()->tid, names[nr] ? names[nr] : "?",
                (long)x[0], x[1], x[2], r);
    /* Set a new descriptor's close-on-exec flag, as its call asked. */
    if (r >= 0 && r < FDS) {
        long fl = -1;
        switch (nr) {
        case 56: fl = (long)(x[2] & 02000000); break;           /* openat: O_CLOEXEC */
        case 19: case 198: fl = (long)(x[1] & 02000000); break; /* eventfd2, socket */
        case 242: fl = (long)(x[3] & 02000000); break;          /* accept4 */
        case 279: fl = (long)(x[1] & 1); break;                 /* memfd_create: MFD_CLOEXEC */
        case 23: case 202: fl = 0; break;                       /* dup, accept */
        case 24: fl = (long)(x[2] & 02000000); break;           /* dup3 */
        }
        if (fl >= 0)
            fds[r].cloexec = fl != 0;
    }
    if (r != SWITCHED && nr == 73 && thread_current()->mask_restore) {
        thread_current()->sigmask = thread_current()->saved_mask;   /* ppoll done: restore mask */
        thread_current()->mask_restore = 0;
    }
    if (r != SWITCHED) {
        thread_current()->sock_deadline = 0;    /* any socket call's wait is over */
        x[0] = (uint64_t)r;
        thread_current()->last_ret = (uint64_t)r;
    }
}
