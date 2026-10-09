/* unix.c: AF_UNIX stream sockets, bound to a path to listen on and connect
 * to, and socketpair.
 *
 * The box's SSHD listens on one of these (/run/rosgd-cli.sock) for the
 * sessions that sshd's riscos-cli starts, and programs use socketpair.
 * A connection consists of two sockets, each with a ring buffer that its
 * peer writes into.  When the peer closes or shuts down its end, the reader
 * sees end of file.  The path is not a file: connect looks it up in a
 * table of bound paths.  There is therefore no file to unlink; unlinking
 * the path returns ENOENT, which the box ignores.  These sockets do not
 * pass descriptors or credentials (SCM_RIGHTS), and they do not support
 * datagrams. */
#include "hal.h"

#define EAGAIN 11
#define EBADF 9
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define EMFILE 24
#define EPIPE 32
#define ENOENT 2
#define EOPNOTSUPP 95
#define EADDRINUSE 98
#define EISCONN 106
#define ENOTCONN 107
#define ECONNREFUSED 111

#define UNIXES 64
#define RING (16 * 1024)
#define BACKLOG 16

static struct usock {
    int used, refs, nonblock;
    int peer;                                   /* the other end, or -1 if none */
    int peer_gone, wr_shut, rd_shut;            /* peer closed; writing shut; reading shut */
    int listening, queue[BACKLOG], nqueue;
    int pending;                                /* created by connect, not yet accepted */
    int pid;                                    /* creating process, for SO_PEERCRED */
    char path[108];
    unsigned head, count;
    char *ring;
} us[UNIXES];

static uint64_t key(struct usock *u) { return (uint64_t)u; }
static void wake(struct usock *u)
{
    futex_wake(key(u), 1L << 30);
    futex_wake(KEY_POLL, 1L << 30);
}

static struct usock *U(int i)
{
    return i >= 0 && i < UNIXES && us[i].used ? &us[i] : 0;
}

static int new_sock(void)
{
    for (int i = 0; i < UNIXES; i++)
        if (!us[i].used) {
            struct usock *u = &us[i];
            uint64_t pa = pages_alloc_run(RING / PAGE_SIZE);
            if (!pa)
                return -ENOMEM;
            memset(u, 0, sizeof *u);
            u->used = 1, u->refs = 1, u->peer = -1;
            u->ring = pa_to_va(pa);
            u->pid = thread_current()->proc->pid;
            return i;
        }
    return -EMFILE;
}

long unix_socket(long type)
{
    if ((type & 0xF) != 1)                      /* only SOCK_STREAM is supported */
        return -EOPNOTSUPP;
    int i = new_sock();
    if (i >= 0)
        us[i].nonblock = (type & 04000) != 0;
    return i;
}

/* socketpair: create two connected sockets and return them in pair[0] and
 * pair[1]. */
void unix_close(int i);

long unix_pair(long type, int pair[2])
{
    long a = unix_socket(type), b = a < 0 ? a : unix_socket(type);
    if (b < 0) {
        if (a >= 0)
            unix_close((int)a);
        return b;
    }
    us[a].peer = (int)b, us[b].peer = (int)a;
    pair[0] = (int)a, pair[1] = (int)b;
    return 0;
}

void unix_hold(int i)
{
    if (U(i))
        us[i].refs++;
}

void unix_close(int i)
{
    struct usock *u = U(i);
    if (!u || --u->refs > 0)
        return;
    struct usock *p = U(u->peer);
    if (p) {
        p->peer_gone = 1;
        wake(p);
    }
    for (int k = 0; k < u->nqueue; k++)         /* close connections never accepted */
        unix_close(u->queue[k]);
    uint64_t pa = va_to_pa(u->ring);
    for (uint64_t k = 0; k < RING / PAGE_SIZE; k++)
        page_free(pa + k * PAGE_SIZE);
    u->used = 0;
}

int unix_nonblock(int i, int set)
{
    struct usock *u = U(i);
    if (!u)
        return 0;
    if (set >= 0)
        u->nonblock = set;
    return u->nonblock;
}

static long get_path(uint64_t va, uint64_t len, char *out)
{
    if (len < 3 || len > 110)
        return -EINVAL;
    uint8_t a[110];
    memset(a, 0, sizeof a);
    if (copy_from_box(a, va, len))
        return -EFAULT;
    if ((a[0] | a[1] << 8) != 1)                /* AF_UNIX */
        return -EINVAL;
    size_t n = len - 2;
    memcpy(out, a + 2, n);
    out[n < 107 ? n : 107] = 0;
    if (!out[0] && n > 1)                       /* abstract name: it follows the 0 byte */
        out[0] = '@';
    return 0;
}

static struct usock *bound(const char *path)
{
    for (int i = 0; i < UNIXES; i++)
        if (us[i].used && us[i].path[0] && !strcmp(us[i].path, path) && !us[i].pending && us[i].peer < 0)
            return &us[i];
    return 0;
}

long unix_bind(int i, uint64_t va, uint64_t len)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    char path[108];
    long e = get_path(va, len, path);
    if (e)
        return e;
    if (bound(path))
        return -EADDRINUSE;
    memcpy(u->path, path, sizeof path);
    return 0;
}

long unix_listen(int i)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    if (!u->path[0])
        return -EINVAL;
    u->listening = 1;
    return 0;
}

long unix_connect(int i, uint64_t va, uint64_t len)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    if (u->peer >= 0)
        return -EISCONN;
    char path[108];
    long e = get_path(va, len, path);
    if (e)
        return e;
    struct usock *l = bound(path);
    if (!l)
        return -ENOENT;
    if (!l->listening || l->nqueue == BACKLOG)
        return -ECONNREFUSED;
    int s = new_sock();
    if (s < 0)
        return s;
    struct usock *srv = &us[s];
    srv->refs = 0, srv->pending = 1;
    memcpy(srv->path, path, sizeof path);
    srv->peer = i, u->peer = s;
    srv->pid = l->pid;
    l->queue[l->nqueue++] = s;
    wake(l);
    return 0;
}

long unix_accept(struct frame *fr, int i)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    if (!u->listening)
        return -EINVAL;
    if (!u->nqueue) {
        if (u->nonblock || !fr)
            return -EAGAIN;
        thread_block_restart(fr, key(u), 0);
        return SWITCHED;
    }
    int s = u->queue[0];
    memmove(u->queue, u->queue + 1, (size_t)--u->nqueue * sizeof u->queue[0]);
    us[s].refs = 1, us[s].pending = 0;
    return s;
}

long unix_send(struct frame *fr, int i, uint64_t buf, uint64_t n, int dontwait)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    struct usock *p = U(u->peer);
    if (u->peer < 0 && !u->peer_gone)
        return -ENOTCONN;
    if (!p || u->peer_gone || u->wr_shut || p->rd_shut)
        return -EPIPE;
    if (p->count == RING) {
        if (u->nonblock || dontwait || !fr)
            return -EAGAIN;
        thread_block_restart(fr, key(u), 0);
        return SWITCHED;
    }
    uint64_t k = 0;
    while (k < n && p->count < RING) {
        unsigned at = (p->head + p->count) % RING;
        uint64_t c = RING - p->count;
        if (c > RING - at)
            c = RING - at;
        if (c > n - k)
            c = n - k;
        if (copy_from_box(p->ring + at, buf + k, c))
            return k ? (long)k : -EFAULT;
        p->count += (unsigned)c, k += c;
    }
    wake(p);
    return (long)k;
}

long unix_recv(struct frame *fr, int i, uint64_t buf, uint64_t n, int flags)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    if (!u->count) {
        if (u->peer_gone || u->rd_shut || (u->peer >= 0 && U(u->peer) && U(u->peer)->wr_shut))
            return 0;                           /* end of file */
        if (u->peer < 0)
            return -ENOTCONN;
        if (u->nonblock || (flags & 0x40) || !fr)   /* MSG_DONTWAIT */
            return -EAGAIN;
        thread_block_restart(fr, key(u), 0);
        return SWITCHED;
    }
    uint64_t k = 0;
    unsigned head = u->head, count = u->count;
    while (k < n && count) {
        uint64_t c = RING - head;
        if (c > count)
            c = count;
        if (c > n - k)
            c = n - k;
        if (copy_to_box(buf + k, u->ring + head, c))
            return k ? (long)k : -EFAULT;
        head = (head + (unsigned)c) % RING, count -= (unsigned)c, k += c;
    }
    if (!(flags & 2)) {                         /* not MSG_PEEK */
        u->head = head, u->count = count;
        struct usock *p = U(u->peer);
        if (p)
            wake(p);                            /* there is now room for the writer */
    }
    return (long)k;
}

unsigned unix_poll(int i)
{
    struct usock *u = U(i);
    if (!u)
        return 8;
    if (u->listening)
        return u->nqueue ? 1 : 0;
    unsigned r = 0;
    struct usock *p = U(u->peer);
    if (u->count || u->peer_gone || (p && p->wr_shut))
        r |= 1;                                 /* POLLIN */
    if (p && !u->peer_gone && p->count < RING)
        r |= 4;                                 /* POLLOUT */
    if (u->peer_gone)
        r |= 16;                                /* POLLHUP */
    return r;
}

long unix_shutdown(int i, long how)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    if (u->peer < 0)
        return -ENOTCONN;
    if (how != 1)
        u->rd_shut = 1;
    if (how != 0)
        u->wr_shut = 1;
    struct usock *p = U(u->peer);
    if (p)
        wake(p);
    wake(u);
    return 0;
}

long unix_name(int i, uint64_t va, uint64_t lenp, int peer)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    if (peer && u->peer < 0)
        return -ENOTCONN;
    const char *path = u->path;
    uint8_t a[110];
    memset(a, 0, sizeof a);
    a[0] = 1;
    size_t n = strlen(path);
    memcpy(a + 2, path, n);
    uint32_t len, want = (uint32_t)(2 + n + 1);
    if (copy_from_box(&len, lenp, 4))
        return -EFAULT;
    if (copy_to_box(va, a, len < want ? len : want) || copy_to_box(lenp, &want, 4))
        return -EFAULT;
    return 0;
}

long unix_getopt(int i, long level, long name, uint64_t val, uint64_t lenp)
{
    struct usock *u = U(i);
    if (!u)
        return -EBADF;
    if (level == 1 && name == 17) {             /* SO_PEERCRED: pid, uid, gid */
        struct usock *p = U(u->peer);
        int32_t c[3] = { p ? p->pid : 0, 0, 0 };
        uint32_t len = 12;
        return copy_to_box(val, c, 12) || copy_to_box(lenp, &len, 4) ? -EFAULT : 0;
    }
    if (level == 1 && (name == 3 || name == 4)) {   /* SO_TYPE, SO_ERROR */
        int32_t v = name == 3 ? 1 : 0;
        uint32_t len = 4;
        return copy_to_box(val, &v, 4) || copy_to_box(lenp, &len, 4) ? -EFAULT : 0;
    }
    return -92;                                 /* ENOPROTOOPT */
}

long unix_fionread(int i)
{
    struct usock *u = U(i);
    return u ? (long)u->count : -EBADF;
}
