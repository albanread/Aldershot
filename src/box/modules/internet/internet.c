/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* internet.c -- the Internet module, rewritten for ROSGD as a native module.
 *
 * RISC OS 5's Internet module is a 4.4BSD TCP/IP stack with a SWI interface
 * on top. Under ROSGD the stack is Linux's, so what is left to write is the
 * interface: the Socket_* SWIs (chunk &41200), each the BSD call of its name,
 * answered with the host's own socket calls. It is new code, written from
 * the documented Socket_* interface (Networking/AUN/Internet's cmhg header
 * and riscos/c/socket_swi, and TCPIPLibs' headers). It does not translate
 * RISC OS's stack, because an OS service is reimplemented.
 *
 * What clients depend on, this keeps:
 *
 *   - RISC OS's numbers. Socket numbers are 0 to 255, machine-wide.
 *     Constants, option names, flags, ioctls and errno values are 4.4BSD's,
 *     and errors are &20E00 + errno. Every one is translated here, by name,
 *     to the host's own, and never assumed equal.
 *   - RISC OS's structures. A sockaddr starts with a length byte and a
 *     family byte (4.4BSD) or a 16-bit family (4.3BSD, the unsuffixed Accept,
 *     Recvfrom, Recvmsg, Sendmsg, Getpeername, Getsockname). Timevals and
 *     iovecs are 32-bit words. An fd_set is 256 bits.
 *
 * The host is Linux in the guest, and macOS or Linux in the hosted build, so
 * the code uses only the host's symbols, and getifaddrs() for interfaces.
 *
 * State: the table from socket numbers to host descriptors is in the RMA,
 * through the private word (module.h). A descriptor belongs to the process.
 * Every task is a thread of it (task.h), so the tasks share the table, as
 * RISC OS's tasks share sockets. Tasks that were processes would need to
 * share it with CLONE_FILES.
 *
 * Every call that may wait releases the personality lock for the wait
 * (ROS_BLOCKING), so background work goes on meanwhile. A socket with
 * FIOASYNC set is watched by the runtime's pump. When input comes, the
 * Internet event (19) is generated at the next safe point, and the watch
 * re-arms when the input is read.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "internet.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/vector.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0          /* macOS: SO_NOSIGPIPE, set at creation */
#endif

#define SOCKETS      256        /* SOCKTABSIZE, and FD_SETSIZE */
#define VERSION      568        /* the Internet module whose interface this is */
#define ERROR_BASE   0x20E00u   /* DCI4ERRORBLOCK */
#define MAX_IOV      1024

/* ---- RISC OS's numbers (TCPIPLibs' headers: 4.4BSD) ---------------------- */

enum { R_AF_UNIX = 1, R_AF_INET = 2, R_AF_INET6 = 28, R_AF_MAX = 33 };
#define R_SOL_SOCKET 0xFFFFu
enum { R_IPPROTO_IP = 0, R_IPPROTO_TCP = 6, R_IPPROTO_IPV6 = 41 };

enum {
    R_EPERM = 1, R_ENOENT = 2, R_EINTR = 4, R_EIO = 5, R_EBADF = 9, R_ENOMEM = 12,
    R_EACCES = 13, R_EFAULT = 14, R_EBUSY = 16, R_EEXIST = 17, R_EINVAL = 22,
    R_ENFILE = 23, R_EMFILE = 24, R_ENOTTY = 25, R_ENOSPC = 28, R_EPIPE = 32,
    R_EAGAIN = 35, R_EINPROGRESS = 36, R_EALREADY = 37, R_ENOTSOCK = 38,
    R_EDESTADDRREQ = 39, R_EMSGSIZE = 40, R_EPROTOTYPE = 41, R_ENOPROTOOPT = 42,
    R_EPROTONOSUPPORT = 43, R_ESOCKTNOSUPPORT = 44, R_EOPNOTSUPP = 45,
    R_EPFNOSUPPORT = 46, R_EAFNOSUPPORT = 47, R_EADDRINUSE = 48, R_EADDRNOTAVAIL = 49,
    R_ENETDOWN = 50, R_ENETUNREACH = 51, R_ENETRESET = 52, R_ECONNABORTED = 53,
    R_ECONNRESET = 54, R_ENOBUFS = 55, R_EISCONN = 56, R_ENOTCONN = 57,
    R_ESHUTDOWN = 58, R_ETOOMANYREFS = 59, R_ETIMEDOUT = 60, R_ECONNREFUSED = 61,
    R_ELOOP = 62, R_ENAMETOOLONG = 63, R_EHOSTDOWN = 64, R_EHOSTUNREACH = 65,
};

/* ---- the state, in the RMA ---------------------------------------------- */

struct workspace {
    int32_t fd[SOCKETS];        /* the host's descriptor, or -1: free */
    uint8_t async[SOCKETS];     /* FIOASYNC: Event 19 wanted (not yet sent) */
};

struct ros_module internet_module;

static struct workspace *ws(void)
{
    return ros_ptr(ros_ld32(internet_module.private_word));
}

/* ---- errors --------------------------------------------------------------- */

static uint32_t bsd_errno(int e)
{
    switch (e) {
    case EPERM: return R_EPERM;             case ENOENT: return R_ENOENT;
    case EINTR: return R_EINTR;             case EIO: return R_EIO;
    case EBADF: return R_EBADF;             case ENOMEM: return R_ENOMEM;
    case EACCES: return R_EACCES;           case EFAULT: return R_EFAULT;
    case EBUSY: return R_EBUSY;             case EEXIST: return R_EEXIST;
    case EINVAL: return R_EINVAL;           case ENFILE: return R_ENFILE;
    case EMFILE: return R_EMFILE;           case ENOTTY: return R_ENOTTY;
    case ENOSPC: return R_ENOSPC;           case EPIPE: return R_EPIPE;
    case EAGAIN: return R_EAGAIN;           case EINPROGRESS: return R_EINPROGRESS;
    case EALREADY: return R_EALREADY;       case ENOTSOCK: return R_ENOTSOCK;
    case EDESTADDRREQ: return R_EDESTADDRREQ; case EMSGSIZE: return R_EMSGSIZE;
    case EPROTOTYPE: return R_EPROTOTYPE;   case ENOPROTOOPT: return R_ENOPROTOOPT;
    case EPROTONOSUPPORT: return R_EPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return R_ESOCKTNOSUPPORT;
    case EOPNOTSUPP: return R_EOPNOTSUPP;   case EPFNOSUPPORT: return R_EPFNOSUPPORT;
    case EAFNOSUPPORT: return R_EAFNOSUPPORT; case EADDRINUSE: return R_EADDRINUSE;
    case EADDRNOTAVAIL: return R_EADDRNOTAVAIL; case ENETDOWN: return R_ENETDOWN;
    case ENETUNREACH: return R_ENETUNREACH; case ENETRESET: return R_ENETRESET;
    case ECONNABORTED: return R_ECONNABORTED; case ECONNRESET: return R_ECONNRESET;
    case ENOBUFS: return R_ENOBUFS;         case EISCONN: return R_EISCONN;
    case ENOTCONN: return R_ENOTCONN;       case ESHUTDOWN: return R_ESHUTDOWN;
    case ETOOMANYREFS: return R_ETOOMANYREFS; case ETIMEDOUT: return R_ETIMEDOUT;
    case ECONNREFUSED: return R_ECONNREFUSED; case ELOOP: return R_ELOOP;
    case ENAMETOOLONG: return R_ENAMETOOLONG; case EHOSTDOWN: return R_EHOSTDOWN;
    case EHOSTUNREACH: return R_EHOSTUNREACH;
    default: return R_EINVAL;
    }
}

/* An error in RISC OS's numbering, from a host errno. */
static os_error *fail(int host_errno)
{
    return ros_error(ERROR_BASE + bsd_errno(host_errno), "%s", strerror(host_errno));
}

/* An error that is RISC OS's own, with no host errno behind it. */
static os_error *fail_bsd(uint32_t e, const char *text)
{
    return ros_error(ERROR_BASE + e, "%s", text);
}

static void event(uint32_t s, uint32_t reason);

/* A send's error; a broken connection on an FIOASYNC socket also raises the
 * Internet event, as SIGPIPE did. */
static os_error *broken(uint32_t s, int e)
{
    if (e == EPIPE && s < SOCKETS && ws()->async[s])
        event(s, 3);
    return fail(e);
}

/* ---- sockets ------------------------------------------------------------- */

static int host_fd(uint32_t s)
{
    return s < SOCKETS ? ws()->fd[s] : -1;
}

#define SOCKET(s, fd)                                              \
    int fd = host_fd(s);                                           \
    if (fd < 0)                                                    \
        return fail_bsd(R_EBADF, "Bad file descriptor")

/* A host descriptor into the table: its socket number, or -1 when full. */
static int install(int fd)
{
    struct workspace *w = ws();
    for (int s = 0; s < SOCKETS; s++)
        if (w->fd[s] < 0) {
            w->fd[s] = fd;
            w->async[s] = 0;
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            return s;
        }
    return -1;
}

/* ---- the Internet event ---------------------------------------------------
 *
 * Event 19. R1 is the reason: 1 for input (SIGIO's), 2 for urgent data
 * (SIGURG's), 3 for a broken connection (SIGPIPE's). R2 is the socket. R3
 * is its local port for an Internet socket (Kernel/hdr/RISCOS; the stack's
 * lib/c/unixenv).
 */
#define EVENT_INTERNET 19u
enum { EV_ASYNC = 1, EV_URGENT = 2, EV_BROKEN = 3 };

static void event(uint32_t s, uint32_t reason)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    uint32_t port = 0;
    int fd = host_fd(s);
    if (fd >= 0 && getsockname(fd, (struct sockaddr *)&ss, &sl) == 0 &&
        (ss.ss_family == AF_INET || ss.ss_family == AF_INET6))
        port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = EVENT_INTERNET;
    c.r[1] = reason;
    c.r[2] = s;
    c.r[3] = port;
    ros_event_generate(&c);
}

static void async_ready(void *arg, uint32_t revents);

/* Watch an FIOASYNC socket for its next input. */
static void arm(uint32_t s)
{
    int fd = host_fd(s);
    if (fd >= 0 && ws()->async[s])
        ros_watch(fd, POLLIN | POLLPRI, async_ready, (void *)(uintptr_t)s);
}

/* The pump saw input: at a safe point, holding the lock. */
static void async_ready(void *arg, uint32_t revents)
{
    uint32_t s = (uint32_t)(uintptr_t)arg;
    if (host_fd(s) < 0 || !ws()->async[s])
        return;                     /* closed, or no longer asynchronous */
    event(s, revents & POLLPRI ? EV_URGENT : EV_ASYNC);
}

/* After a call that consumed input: the next input raises the event again. */
static void consumed(uint32_t s)
{
    if (s < SOCKETS && ws()->async[s])
        arm(s);
}

/* For modules layered on a socket (AcornSSL): the Linux descriptor behind a
 * RISC OS socket number, or -1 if it is not open. After reading from it
 * directly, the module re-arms the Internet event's watch, as the module's
 * own reads do. */
int ros_internet_fd(uint32_t s)
{
    return host_fd(s);
}

void ros_internet_consumed(uint32_t s)
{
    consumed(s);
}

static int family_from(uint32_t f)
{
    switch (f) {
    case R_AF_UNIX: return AF_UNIX;
    case R_AF_INET: return AF_INET;
    case R_AF_INET6: return AF_INET6;
    default: return -1;
    }
}

static uint32_t family_to(int f)
{
    switch (f) {
    case AF_UNIX: return R_AF_UNIX;
    case AF_INET: return R_AF_INET;
    case AF_INET6: return R_AF_INET6;
    default: return 0;
    }
}

/* A RISC OS sockaddr, either form, as the host's.  4.4BSD's has a length
 * byte and a family byte; 4.3BSD's a 16-bit family, whose high byte is 0.
 * A zero family byte with a small first byte is 4.3's, as BSD's own
 * compatibility code decides.  After the family, the layouts agree. */
static os_error *addr_in(const uint8_t *p, uint32_t len, struct sockaddr_storage *ss,
                         socklen_t *sl)
{
    if (!p || len < 2 || len > sizeof *ss)
        return fail_bsd(R_EINVAL, "Invalid argument");
    uint32_t fam = p[1] == 0 && p[0] < R_AF_MAX ? p[0] : p[1];
    int host = family_from(fam);
    if (host < 0)
        return fail_bsd(R_EAFNOSUPPORT, "Address family not supported");
    memset(ss, 0, sizeof *ss);
    memcpy((uint8_t *)ss + 2, p + 2, len - 2);
    ss->ss_family = (sa_family_t)host;
#ifdef __APPLE__
    ss->ss_len = (uint8_t)len;
#endif
    *sl = len;
    return NULL;
}

/* The host's sockaddr as RISC OS's, in the form the SWI promises: what fits
 * in *len bytes, and *len set to its whole size. */
static void addr_out(const struct sockaddr_storage *ss, socklen_t sl, uint8_t *p,
                     uint32_t *len, int bsd44)
{
    if (!p || !len)
        return;
    uint8_t tmp[sizeof *ss];
    if (sl > sizeof tmp)
        sl = sizeof tmp;
    memcpy(tmp, ss, sl);
    uint32_t fam = family_to(ss->ss_family);
    if (bsd44) {
        tmp[0] = (uint8_t)sl;
        tmp[1] = (uint8_t)fam;
    } else {
        tmp[0] = (uint8_t)fam;
        tmp[1] = 0;
    }
    memcpy(p, tmp, sl < *len ? sl : *len);
    *len = sl;
}

/* send and recv flags */
static int flags_in(uint32_t f)
{
    int h = 0;
    if (f & 0x01) h |= MSG_OOB;
    if (f & 0x02) h |= MSG_PEEK;
    if (f & 0x04) h |= MSG_DONTROUTE;
    if (f & 0x08) h |= MSG_EOR;
    if (f & 0x40) h |= MSG_WAITALL;
    if (f & 0x80) h |= MSG_DONTWAIT;
    return h;
}

static uint32_t flags_out(int h)
{
    uint32_t f = 0;
    if (h & MSG_OOB) f |= 0x01;
    if (h & MSG_EOR) f |= 0x08;
    if (h & MSG_TRUNC) f |= 0x10;
    if (h & MSG_CTRUNC) f |= 0x20;
    return f;
}

/* ---- the SWIs ------------------------------------------------------------ */

os_error *xsocket_creat(uint32_t domain, uint32_t type, uint32_t protocol, uint32_t *sock)
{
    int fam = family_from(domain);
    if (fam < 0)
        return fail_bsd(R_EAFNOSUPPORT, "Address family not supported");
    /* SOCK_STREAM 1, DGRAM 2, RAW 3, SEQPACKET 5: the same everywhere. */
    int fd = socket(fam, (int)type, (int)protocol);
    if (fd < 0)
        return fail(errno);
    int n = install(fd);
    if (n < 0) {
        close(fd);
        return fail_bsd(R_EMFILE, "Too many open sockets");
    }
    *sock = (uint32_t)n;
    return NULL;
}

os_error *xsocket_bind(uint32_t s, const uint8_t *name, uint32_t namelen)
{
    SOCKET(s, fd);
    struct sockaddr_storage ss;
    socklen_t sl;
    os_error *e = addr_in(name, namelen, &ss, &sl);
    if (e)
        return e;
    return bind(fd, (struct sockaddr *)&ss, sl) < 0 ? fail(errno) : NULL;
}

os_error *xsocket_listen(uint32_t s, uint32_t backlog)
{
    SOCKET(s, fd);
    return listen(fd, (int)backlog) < 0 ? fail(errno) : NULL;
}

static os_error *accept_as(uint32_t s, uint8_t *addr, uint32_t *addrlen, uint32_t *ns,
                           int bsd44)
{
    SOCKET(s, fd);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int nfd;
    ROS_BLOCKING(nfd = accept(fd, (struct sockaddr *)&ss, &sl));
    consumed(s);
    if (nfd < 0)
        return fail(errno);
    int n = install(nfd);
    if (n < 0) {
        close(nfd);
        return fail_bsd(R_EMFILE, "Too many open sockets");
    }
    addr_out(&ss, sl, addr, addrlen, bsd44);
    *ns = (uint32_t)n;
    return NULL;
}

os_error *xsocket_accept(uint32_t s, uint8_t *addr, uint32_t *addrlen, uint32_t *ns)
{
    return accept_as(s, addr, addrlen, ns, 0);
}

os_error *xsocket_accept_1(uint32_t s, uint8_t *addr, uint32_t *addrlen, uint32_t *ns)
{
    return accept_as(s, addr, addrlen, ns, 1);
}

os_error *xsocket_connect(uint32_t s, const uint8_t *name, uint32_t namelen)
{
    SOCKET(s, fd);
    struct sockaddr_storage ss;
    socklen_t sl;
    os_error *e = addr_in(name, namelen, &ss, &sl);
    if (e)
        return e;
    int r;
    ROS_BLOCKING(r = connect(fd, (struct sockaddr *)&ss, sl));
    return r < 0 ? fail(errno) : NULL;
}

os_error *xsocket_recv(uint32_t s, uint8_t *buf, uint32_t len, uint32_t flags, uint32_t *n)
{
    SOCKET(s, fd);
    ssize_t r;
    ROS_BLOCKING(r = recv(fd, buf, len, flags_in(flags)));
    consumed(s);
    if (r < 0)
        return fail(errno);
    *n = (uint32_t)r;
    return NULL;
}

static os_error *recvfrom_as(uint32_t s, uint8_t *buf, uint32_t len, uint32_t flags,
                             uint8_t *from, uint32_t *fromlen, uint32_t *n, int bsd44)
{
    SOCKET(s, fd);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(&ss, 0, sizeof ss);
    ssize_t r;
    ROS_BLOCKING(r = recvfrom(fd, buf, len, flags_in(flags), (struct sockaddr *)&ss, &sl));
    consumed(s);
    if (r < 0)
        return fail(errno);
    if (from && fromlen)
        addr_out(&ss, sl, from, fromlen, bsd44);
    *n = (uint32_t)r;
    return NULL;
}

os_error *xsocket_recvfrom(uint32_t s, uint8_t *buf, uint32_t len, uint32_t flags,
                           uint8_t *from, uint32_t *fromlen, uint32_t *n)
{
    return recvfrom_as(s, buf, len, flags, from, fromlen, n, 0);
}

os_error *xsocket_recvfrom_1(uint32_t s, uint8_t *buf, uint32_t len, uint32_t flags,
                             uint8_t *from, uint32_t *fromlen, uint32_t *n)
{
    return recvfrom_as(s, buf, len, flags, from, fromlen, n, 1);
}

os_error *xsocket_send(uint32_t s, const uint8_t *buf, uint32_t len, uint32_t flags,
                       uint32_t *n)
{
    SOCKET(s, fd);
    ssize_t r;
    ROS_BLOCKING(r = send(fd, buf, len, flags_in(flags) | MSG_NOSIGNAL));
    if (r < 0)
        return broken(s, errno);
    *n = (uint32_t)r;
    return NULL;
}

os_error *xsocket_sendto(uint32_t s, const uint8_t *buf, uint32_t len, uint32_t flags,
                         const uint8_t *to, uint32_t tolen, uint32_t *n)
{
    SOCKET(s, fd);
    struct sockaddr_storage ss;
    socklen_t sl = 0;
    if (to) {
        os_error *e = addr_in(to, tolen, &ss, &sl);
        if (e)
            return e;
    }
    ssize_t r;
    ROS_BLOCKING(r = sendto(fd, buf, len, flags_in(flags) | MSG_NOSIGNAL,
                            to ? (struct sockaddr *)&ss : NULL, sl));
    if (r < 0)
        return broken(s, errno);
    *n = (uint32_t)r;
    return NULL;
}

/* iovecs: RISC OS's are two words, base and length. */
static os_error *iov_in(const uint32_t *iov, uint32_t count, struct iovec *out)
{
    if (count > MAX_IOV || (count && !iov))
        return fail_bsd(R_EINVAL, "Invalid argument");
    for (uint32_t i = 0; i < count; i++) {
        out[i].iov_base = iov[2 * i] ? ros_ptr(iov[2 * i]) : NULL;
        out[i].iov_len = iov[2 * i + 1];
    }
    return NULL;
}

/* A msghdr, either form. 4.3's has six words: name, namelen, iov, iovlen,
 * accrights, accrightslen. 4.4's has seven: name, namelen, iov, iovlen,
 * control, controllen, flags. Rights and control data are not carried. */
enum { M_NAME, M_NAMELEN, M_IOV, M_IOVLEN, M_CONTROL, M_CONTROLLEN, M_FLAGS };

static os_error *msg_io(uint32_t s, uint32_t *m, uint32_t flags, uint32_t *n, int bsd44,
                        int sending)
{
    SOCKET(s, fd);
    struct iovec iov[MAX_IOV];     /* on the stack: background work may call in */
    os_error *e = iov_in(m[M_IOV] ? ros_ptr(m[M_IOV]) : NULL, m[M_IOVLEN], iov);
    if (e)
        return e;
    struct sockaddr_storage ss;
    struct msghdr h = { .msg_iov = iov, .msg_iovlen = m[M_IOVLEN] };
    if (sending) {
        socklen_t sl = 0;
        if (m[M_NAME]) {
            if ((e = addr_in(ros_ptr(m[M_NAME]), m[M_NAMELEN], &ss, &sl)))
                return e;
            h.msg_name = &ss;
            h.msg_namelen = sl;
        }
        ssize_t r;
        ROS_BLOCKING(r = sendmsg(fd, &h, flags_in(flags) | MSG_NOSIGNAL));
        if (r < 0)
            return broken(s, errno);
        *n = (uint32_t)r;
        return NULL;
    }
    memset(&ss, 0, sizeof ss);
    if (m[M_NAME]) {
        h.msg_name = &ss;
        h.msg_namelen = sizeof ss;
    }
    ssize_t r;
    ROS_BLOCKING(r = recvmsg(fd, &h, flags_in(flags)));
    consumed(s);
    if (r < 0)
        return fail(errno);
    if (m[M_NAME]) {
        uint32_t len = m[M_NAMELEN];
        addr_out(&ss, h.msg_namelen, ros_ptr(m[M_NAME]), &len, bsd44);
        m[M_NAMELEN] = len;
    }
    m[M_CONTROLLEN] = 0;
    if (bsd44)
        m[M_FLAGS] = flags_out(h.msg_flags);
    *n = (uint32_t)r;
    return NULL;
}

os_error *xsocket_recvmsg(uint32_t s, uint32_t *msg, uint32_t flags, uint32_t *n)
{
    return msg_io(s, msg, flags, n, 0, 0);
}

os_error *xsocket_recvmsg_1(uint32_t s, uint32_t *msg, uint32_t flags, uint32_t *n)
{
    return msg_io(s, msg, flags, n, 1, 0);
}

os_error *xsocket_sendmsg(uint32_t s, const uint32_t *msg, uint32_t flags, uint32_t *n)
{
    return msg_io(s, (uint32_t *)msg, flags, n, 0, 1);
}

os_error *xsocket_sendmsg_1(uint32_t s, const uint32_t *msg, uint32_t flags, uint32_t *n)
{
    return msg_io(s, (uint32_t *)msg, flags, n, 1, 1);
}

os_error *xsocket_shutdown(uint32_t s, uint32_t how)
{
    SOCKET(s, fd);
    return shutdown(fd, (int)how) < 0 ? fail(errno) : NULL;   /* 0, 1, 2 everywhere */
}

/* ---- options ------------------------------------------------------------- */

enum { OPT_INT, OPT_TIMEVAL, OPT_LINGER, OPT_ERROR, OPT_RAW };

/* A RISC OS level and option as the host's: 0, or -1 if the host has none. */
static int option(uint32_t level, uint32_t name, int *hl, int *hn, int *kind)
{
    *kind = OPT_INT;
    if (level == R_SOL_SOCKET) {
        *hl = SOL_SOCKET;
        switch (name) {
        case 0x0001: *hn = SO_DEBUG; return 0;
        case 0x0002: *hn = SO_ACCEPTCONN; return 0;
        case 0x0004: *hn = SO_REUSEADDR; return 0;
        case 0x0008: *hn = SO_KEEPALIVE; return 0;
        case 0x0010: *hn = SO_DONTROUTE; return 0;
        case 0x0020: *hn = SO_BROADCAST; return 0;
        case 0x0080: *hn = SO_LINGER; *kind = OPT_LINGER; return 0;
        case 0x0100: *hn = SO_OOBINLINE; return 0;
        case 0x0200: *hn = SO_REUSEPORT; return 0;
        case 0x1001: *hn = SO_SNDBUF; return 0;
        case 0x1002: *hn = SO_RCVBUF; return 0;
        case 0x1003: *hn = SO_SNDLOWAT; return 0;
        case 0x1004: *hn = SO_RCVLOWAT; return 0;
        case 0x1005: *hn = SO_SNDTIMEO; *kind = OPT_TIMEVAL; return 0;
        case 0x1006: *hn = SO_RCVTIMEO; *kind = OPT_TIMEVAL; return 0;
        case 0x1007: *hn = SO_ERROR; *kind = OPT_ERROR; return 0;
        case 0x1008: *hn = SO_TYPE; return 0;
        default: return -1;
        }
    }
    if (level == R_IPPROTO_IP) {
        *hl = IPPROTO_IP;
        *kind = OPT_RAW;
        switch (name) {
        case 2: *hn = IP_HDRINCL; *kind = OPT_INT; return 0;
        case 3: *hn = IP_TOS; *kind = OPT_INT; return 0;
        case 4: *hn = IP_TTL; *kind = OPT_INT; return 0;
        case 9: *hn = IP_MULTICAST_IF; return 0;            /* struct in_addr */
        case 10: *hn = IP_MULTICAST_TTL; return 0;          /* u_char */
        case 11: *hn = IP_MULTICAST_LOOP; return 0;         /* u_char */
        case 12: *hn = IP_ADD_MEMBERSHIP; return 0;         /* struct ip_mreq */
        case 13: *hn = IP_DROP_MEMBERSHIP; return 0;
        default: return -1;
        }
    }
    if (level == R_IPPROTO_TCP) {
        *hl = IPPROTO_TCP;
        switch (name) {
        case 1: *hn = TCP_NODELAY; return 0;
        case 2: *hn = TCP_MAXSEG; return 0;
        default: return -1;
        }
    }
    if (level == R_IPPROTO_IPV6) {
        *hl = IPPROTO_IPV6;
        if (name == 27) {                                   /* IPV6_V6ONLY */
            *hn = IPV6_V6ONLY;
            return 0;
        }
        return -1;
    }
    return -1;
}

os_error *xsocket_setsockopt(uint32_t s, uint32_t level, uint32_t optname,
                             const uint8_t *optval, uint32_t optlen)
{
    SOCKET(s, fd);
    int hl, hn, kind;
    if (option(level, optname, &hl, &hn, &kind) < 0)
        return fail_bsd(R_ENOPROTOOPT, "Protocol not available");
    if (!optval && optlen)
        return fail_bsd(R_EFAULT, "Bad address");
    if (kind == OPT_TIMEVAL) {
        if (optlen < 8)
            return fail_bsd(R_EINVAL, "Invalid argument");
        int32_t tv32[2];
        memcpy(tv32, optval, 8);
        struct timeval tv = { .tv_sec = tv32[0], .tv_usec = tv32[1] };
        return setsockopt(fd, hl, hn, &tv, sizeof tv) < 0 ? fail(errno) : NULL;
    }
    return setsockopt(fd, hl, hn, optval, optlen) < 0 ? fail(errno) : NULL;
}

os_error *xsocket_getsockopt(uint32_t s, uint32_t level, uint32_t optname, uint8_t *optval,
                             uint32_t *optlen)
{
    SOCKET(s, fd);
    int hl, hn, kind;
    if (option(level, optname, &hl, &hn, &kind) < 0)
        return fail_bsd(R_ENOPROTOOPT, "Protocol not available");
    if (!optval || !optlen)
        return fail_bsd(R_EFAULT, "Bad address");
    union {
        int i;
        struct timeval tv;
        struct linger l;
        uint8_t raw[64];
    } v;
    memset(&v, 0, sizeof v);
    socklen_t vl = sizeof v;
    if (getsockopt(fd, hl, hn, &v, &vl) < 0)
        return fail(errno);
    uint8_t out[64];
    uint32_t n;
    if (kind == OPT_TIMEVAL) {
        int32_t tv32[2] = { (int32_t)v.tv.tv_sec, (int32_t)v.tv.tv_usec };
        memcpy(out, tv32, n = 8);
    } else if (kind == OPT_ERROR) {
        int32_t e = v.i ? (int32_t)bsd_errno(v.i) : 0;
        memcpy(out, &e, n = 4);
    } else {
        memcpy(out, &v, n = vl < sizeof out ? vl : sizeof out);   /* ints, linger: the same */
    }
    memcpy(optval, out, n < *optlen ? n : *optlen);
    *optlen = n;
    return NULL;
}

static os_error *name_as(uint32_t s, uint8_t *name, uint32_t *namelen, int bsd44, int peer)
{
    SOCKET(s, fd);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(&ss, 0, sizeof ss);
    if ((peer ? getpeername : getsockname)(fd, (struct sockaddr *)&ss, &sl) < 0)
        return fail(errno);
    addr_out(&ss, sl, name, namelen, bsd44);
    return NULL;
}

os_error *xsocket_getpeername(uint32_t s, uint8_t *name, uint32_t *namelen)
{
    return name_as(s, name, namelen, 0, 1);
}

os_error *xsocket_getpeername_1(uint32_t s, uint8_t *name, uint32_t *namelen)
{
    return name_as(s, name, namelen, 1, 1);
}

os_error *xsocket_getsockname(uint32_t s, uint8_t *name, uint32_t *namelen)
{
    return name_as(s, name, namelen, 0, 0);
}

os_error *xsocket_getsockname_1(uint32_t s, uint8_t *name, uint32_t *namelen)
{
    return name_as(s, name, namelen, 1, 0);
}

os_error *xsocket_close(uint32_t s)
{
    SOCKET(s, fd);
    ros_unwatch(fd);
    ws()->fd[s] = -1;
    ws()->async[s] = 0;
    return close(fd) < 0 ? fail(errno) : NULL;
}

/* ---- select -------------------------------------------------------------- */

static int bit(const uint32_t *set, uint32_t i)
{
    return set && (set[i / 32] >> (i % 32)) & 1;
}

os_error *xsocket_select(uint32_t nfds, uint32_t *readfds, uint32_t *writefds,
                         uint32_t *exceptfds, const uint32_t *timeout, uint32_t *n)
{
    if (nfds > SOCKETS)
        nfds = SOCKETS;
    struct pollfd p[SOCKETS];
    uint32_t which[SOCKETS], np = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        short ev = (short)((bit(readfds, i) ? POLLIN : 0) | (bit(writefds, i) ? POLLOUT : 0) |
                           (bit(exceptfds, i) ? POLLPRI : 0));
        if (!ev)
            continue;
        int fd = host_fd(i);
        if (fd < 0)
            return fail_bsd(R_EBADF, "Bad file descriptor");
        p[np] = (struct pollfd){ .fd = fd, .events = ev };
        which[np++] = i;
    }
    int ms = -1;
    if (timeout) {
        int32_t sec = (int32_t)timeout[0], usec = (int32_t)timeout[1];
        if (sec < 0 || usec < 0 || usec >= 1000000)
            return fail_bsd(R_EINVAL, "Invalid argument");
        ms = sec > 2000000 ? 2000000000 : sec * 1000 + (usec + 999) / 1000;
    }
    int r;
    ROS_BLOCKING(r = poll(p, np, ms));
    if (r < 0)
        return fail(errno);
    /* The sets come back holding only the ready sockets; the count is of
     * set bits, as BSD's select counts. */
    uint32_t words = (nfds + 31) / 32, count = 0;
    for (uint32_t k = 0; k < words; k++) {
        if (readfds) readfds[k] = 0;
        if (writefds) writefds[k] = 0;
        if (exceptfds) exceptfds[k] = 0;
    }
    for (uint32_t k = 0; k < np; k++) {
        uint32_t i = which[k], m = 1u << (i % 32);
        short re = p[k].revents;
        if (readfds && (p[k].events & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR)))
            readfds[i / 32] |= m, count++;
        if (writefds && (p[k].events & POLLOUT) && (re & (POLLOUT | POLLHUP | POLLERR)))
            writefds[i / 32] |= m, count++;
        if (exceptfds && (p[k].events & POLLPRI) && (re & POLLPRI))
            exceptfds[i / 32] |= m, count++;
    }
    *n = count;
    return NULL;
}

/* ---- ioctl --------------------------------------------------------------- *
 *
 * BSD's encoding: direction and length in the top bits, then group and
 * number.  The group and number decide; interface requests are known by
 * both their current numbers and 4.3's.
 */
#define IOC(group, num) (((uint32_t)(group) << 8) | (num))

/* RISC OS's struct ifreq: a 16-byte name, then 16 bytes of union. */
enum { IFR_NAME = 0, IFR_UNION = 16, IFREQ_SIZE = 32 };

static uint32_t if_flags_out(unsigned h)
{
    uint32_t f = h & 0x3FF;     /* UP to ALLMULTI: the same in both */
    if (h & IFF_MULTICAST)
        f |= 0x8000;
    return f;
}

/* One interface's IPv4 address of a kind, from getifaddrs(). */
static os_error *if_query(const char *name, int what, struct sockaddr_storage *ss,
                          unsigned *flags)
{
    struct ifaddrs *all, *a;
    if (getifaddrs(&all) < 0)
        return fail(errno);
    os_error *e = fail_bsd(R_EADDRNOTAVAIL, "Can't assign requested address");
    for (a = all; a; a = a->ifa_next) {
        if (strcmp(a->ifa_name, name) != 0)
            continue;
        if (flags) {
            *flags = a->ifa_flags;
            e = NULL;
            break;
        }
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        const struct sockaddr *sa = what == 0 ? a->ifa_addr : what == 1 ? a->ifa_netmask
                                                                        : a->ifa_broadaddr;
        if (!sa)
            break;
        memset(ss, 0, sizeof *ss);
        memcpy(ss, sa, sizeof(struct sockaddr_in));
        ss->ss_family = AF_INET;
        e = NULL;
        break;
    }
    freeifaddrs(all);
    return e;
}

/* SIOCGIFCONF: an ifconf of a length word and a buffer address, filled
 * with one ifreq per interface's IPv4 address. */
static os_error *if_conf(uint8_t *argp, int bsd44)
{
    uint32_t len = ros_ld32(ros_addr(argp)), buf = ros_ld32(ros_addr(argp) + 4), used = 0;
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0)
        return fail(errno);
    for (struct ifaddrs *a = all; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        if (used + IFREQ_SIZE > len)
            break;
        uint8_t *r = ros_ptr(buf + used);
        memset(r, 0, IFREQ_SIZE);
        strncpy((char *)r + IFR_NAME, a->ifa_name, 15);
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        memcpy(&ss, a->ifa_addr, sizeof(struct sockaddr_in));
        ss.ss_family = AF_INET;
        uint32_t sl = 16;
        addr_out(&ss, sizeof(struct sockaddr_in), r + IFR_UNION, &sl, bsd44);
        used += IFREQ_SIZE;
    }
    freeifaddrs(all);
    ros_st32(ros_addr(argp), used);
    return NULL;
}

os_error *xsocket_ioctl(uint32_t s, uint32_t request, uint8_t *argp)
{
    SOCKET(s, fd);
    uint32_t cmd = request & 0xFFFF;
    int in = !!(request & 0x80000000u), out = !!(request & 0x40000000u);
    /* Every request this module answers reads or writes its argument, whatever
     * its direction bits say. */
    int uses_arg = cmd >> 8 == 'i' || cmd == IOC('f', 126) || cmd == IOC('f', 127) || cmd == IOC('f', 125) ||
                   cmd == IOC('s', 7);
    if ((in || out || uses_arg) && !argp)
        return fail_bsd(R_EFAULT, "Bad address");
    int32_t v;
    char name[IFNAMSIZ];
    if (argp && cmd >> 8 == 'i') {
        memcpy(name, argp + IFR_NAME, 16);
        name[sizeof name - 1] = 0;
    }
    struct sockaddr_storage ss;
    unsigned flags;
    os_error *e;
    uint32_t sl;

    switch (cmd) {
    case IOC('f', 126): {                                   /* FIONBIO */
        memcpy(&v, argp, 4);
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, v ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) < 0)
            return fail(errno);
        return NULL;
    }
    case IOC('f', 127): {                                   /* FIONREAD */
        int avail = 0;
        if (ioctl(fd, FIONREAD, &avail) < 0)
            return fail(errno);
        v = avail;
        memcpy(argp, &v, 4);
        return NULL;
    }
    case IOC('f', 125):                                     /* FIOASYNC */
        memcpy(&v, argp, 4);
        ws()->async[s] = v != 0;
        if (v)
            arm(s);
        else
            ros_unwatch(fd);
        return NULL;
    case IOC('s', 7): {                                     /* SIOCATMARK */
        int mark = 0;
        if (ioctl(fd, SIOCATMARK, &mark) < 0)
            return fail(errno);
        v = mark;
        memcpy(argp, &v, 4);
        return NULL;
    }
    case IOC('i', 17):                                      /* SIOCGIFFLAGS */
        if ((e = if_query(name, 0, &ss, &flags)))
            return e;
        v = (int32_t)if_flags_out(flags);
        memset(argp + IFR_UNION, 0, 16);
        memcpy(argp + IFR_UNION, &v, 4);
        return NULL;
    case IOC('i', 99): case IOC('i', 33):                   /* SIOCGIFADDR */
    case IOC('i', 95): case IOC('i', 37):                   /* SIOCGIFNETMASK */
    case IOC('i', 97): case IOC('i', 35): {                 /* SIOCGIFBRDADDR */
        uint32_t num = cmd & 0xFF;
        int what = num == 99 || num == 33 ? 0 : num == 95 || num == 37 ? 1 : 2;
        if ((e = if_query(name, what, &ss, NULL)))
            return e;
        sl = 16;
        memset(argp + IFR_UNION, 0, 16);
        addr_out(&ss, sizeof(struct sockaddr_in), argp + IFR_UNION, &sl, num > 90);
        return NULL;
    }
    case IOC('i', 96): case IOC('i', 36):                   /* SIOCGIFCONF */
        return if_conf(argp, (cmd & 0xFF) == 96);
    case IOC('i', 51): {                                    /* SIOCGIFMTU */
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        memcpy(ifr.ifr_name, name, sizeof ifr.ifr_name);
        if (ioctl(fd, SIOCGIFMTU, &ifr) < 0)
            return fail(errno);
        v = ifr.ifr_mtu;
        memset(argp + IFR_UNION, 0, 16);
        memcpy(argp + IFR_UNION, &v, 4);
        return NULL;
    }
    default:
        /* Interfaces are configured by Linux (the kernel's DHCP), not
         * through the socket interface: the set requests are refused. */
        return fail_bsd(R_EOPNOTSUPP, "Operation not supported");
    }
}

/* ---- read and write ------------------------------------------------------ */

os_error *xsocket_read(uint32_t s, uint8_t *buf, uint32_t len, uint32_t *n)
{
    return xsocket_recv(s, buf, len, 0, n);
}

os_error *xsocket_write(uint32_t s, const uint8_t *buf, uint32_t len, uint32_t *n)
{
    return xsocket_send(s, buf, len, 0, n);
}

os_error *xsocket_readv(uint32_t s, const uint32_t *iov, uint32_t iovcnt, uint32_t *n)
{
    uint32_t m[7] = { 0, 0, iov ? ros_addr(iov) : 0, iovcnt, 0, 0, 0 };
    return msg_io(s, m, 0, n, 1, 0);
}

os_error *xsocket_writev(uint32_t s, const uint32_t *iov, uint32_t iovcnt, uint32_t *n)
{
    uint32_t m[7] = { 0, 0, iov ? ros_addr(iov) : 0, iovcnt, 0, 0, 0 };
    return msg_io(s, m, 0, n, 1, 1);
}

os_error *xsocket_gettsize(uint32_t *size)
{
    *size = SOCKETS;
    return NULL;
}

os_error *xsocket_version(uint32_t *version)
{
    *version = VERSION;
    return NULL;
}

/* ---- the module ---------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    for (int s = 0; s < SOCKETS; s++)
        w->fd[s] = -1;
    memset(w->async, 0, sizeof w->async);
    ros_st32(m->private_word, ros_addr(w));
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    for (int s = 0; s < SOCKETS; s++)
        if (w->fd[s] >= 0)
            close(w->fd[s]);
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

/* ---- *InetInfo and *InetGateway ------------------------------------------ */

/* The Internet module's own commands (riscos/c/module: do_ininfo and
 * do_ingateway), answered from Linux: the sockets are this module's, the
 * interfaces getifaddrs', packet forwarding /proc/sys/net/ipv4/ip_forward. */

static os_error *print(const char *fmt, ...)
{
    char text[256];
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

#define FORWARDING "/proc/sys/net/ipv4/ip_forward"

static int forwarding(void)
{
    FILE *f = fopen(FORWARDING, "r");
    int c = f ? fgetc(f) : '0';
    if (f)
        fclose(f);
    return c == '1';
}

/* An interface's name as RISC OS's drivers name them: lo0 for the loopback,
 * the rest eth0, eth1... as Linux names them, cut to three letters and a unit
 * number as do_ininfo prints "%.3s%d". */
static void unit_name(const char *linux_name, char out[16])
{
    if (!strcmp(linux_name, "lo")) {
        strcpy(out, "lo0");
        return;
    }
    size_t l = strlen(linux_name), d = l;
    while (d && linux_name[d - 1] >= '0' && linux_name[d - 1] <= '9')
        d--;
    snprintf(out, 16, "%.*s%s", d < 3 ? (int)d : 3, linux_name, d < l ? linux_name + d : "0");
}

static unsigned bsd_flags(unsigned f)
{
    unsigned b = f & (IFF_UP | IFF_BROADCAST | IFF_DEBUG | IFF_LOOPBACK | IFF_POINTOPOINT | IFF_RUNNING |
                      IFF_NOARP | IFF_PROMISC | IFF_ALLMULTI);
    if (f & IFF_MULTICAST)
        b |= 0x8000;
    if ((f & IFF_BROADCAST) && !(f & IFF_LOOPBACK))
        b |= 0x800;                                 /* SIMPLEX: an Ethernet */
    return b;
}

static os_error *cmd_inetinfo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    int res = 0, prot = 0, ifs = 0;
    if (argc == 0) {
        res = 1;
    } else {
        for (const char *t = ros_ptr(tail); *t >= ' '; t++) {
            switch (*t) {
            case 'r': case 'R': res = 1; break;
            case 'p': case 'P': prot = 1; break;
            case 'i': case 'I': ifs = 1; break;
            default: break;
            }
        }
    }
    os_error *e = NULL;
    if (res) {
        int active = 0;
        for (int s = 0; s < SOCKETS; s++)
            active += ws()->fd[s] >= 0;
        e = print("Resource Usage:\n\nSockets active %d\n", active);
    }
    if (!e && prot)
        e = print("%sUse *InetStat -s to examine per-protocol statistics\n", res ? "\n" : "");
    if (!e && ifs) {
        e = print("%sName  MTU    Flags     (ifp)\n", res || prot ? "\n" : "");
        struct ifaddrs *all = NULL;
        int n = 0;
        if (!e && getifaddrs(&all) == 0) {
            char seen[32][IF_NAMESIZE];
            int nseen = 0;
            for (struct ifaddrs *a = all; a && !e; a = a->ifa_next) {
                int dup = 0;
                for (int i = 0; i < nseen; i++)
                    dup |= !strcmp(seen[i], a->ifa_name);
                if (dup || nseen == 32)
                    continue;
                snprintf(seen[nseen++], IF_NAMESIZE, "%s", a->ifa_name);
                long mtu = 0;
                struct ifreq r;
                memset(&r, 0, sizeof r);
                snprintf(r.ifr_name, sizeof r.ifr_name, "%s", a->ifa_name);
                int s = socket(AF_INET, SOCK_DGRAM, 0);
                if (s >= 0 && ioctl(s, SIOCGIFMTU, &r) == 0)
                    mtu = r.ifr_mtu;
                if (s >= 0)
                    close(s);
                char name[16];
                unit_name(a->ifa_name, name);
                e = print("%-5s %-5ld  %08x  %08x\n", name, mtu, bsd_flags(a->ifa_flags),
                          if_nametoindex(a->ifa_name));
                n++;
            }
            freeifaddrs(all);
        }
        if (!e && !n)
            e = print("No interfaces found\n");
    }
    if (!e)
        e = print("%sPacket forwarding %sin operation\n", res || prot || ifs ? "\n" : "",
                  forwarding() ? "" : "not ");
    return e;
}

static os_error *cmd_inetgateway(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    if (argc == 0)
        return print("Packet forwarding %sin operation\n", forwarding() ? "" : "not ");
    char word[8];
    size_t n = 0;
    for (const char *t = ros_ptr(tail); *t > ' ' && n < sizeof word - 1; t++)
        word[n++] = (char)(*t >= 'A' && *t <= 'Z' ? *t + 32 : *t);
    word[n] = 0;
    int on;
    if (!strcmp(word, "on") || !strcmp(word, "1"))
        on = 1;
    else if (!strcmp(word, "off") || !strcmp(word, "0"))
        on = 0;
    else
        return ros_error(0xDC, "Syntax: *InetGateway [on|off]");
    if (forwarding() == on)
        return NULL;
#ifndef __linux__
    return ros_error(ERROR_BASE + R_EPERM, "Packet forwarding is the host's: not available on this host");
#endif
    FILE *f = fopen(FORWARDING, "w");
    int bad = !f;
    if (f) {
        bad = fputc(on ? '1' : '0', f) == EOF;
        bad |= fclose(f) == EOF;
    }
    if (bad)
        return ros_error(ERROR_BASE + R_EPERM, "Packet forwarding cannot be %s here: %s",
                         on ? "turned on" : "turned off", strerror(errno));
    return NULL;
}

static const struct ros_command commands[] = {
    { "InetInfo", ROS_CMD_INFO(0, 3, 0, 0), "Syntax: *InetInfo [r] [p] [i]",
      "*InetInfo shows the Internet module's resource usage (r), protocol statistics (p) "
      "and interfaces (i).", cmd_inetinfo },
    { "InetGateway", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *InetGateway [on|off]",
      "*InetGateway turns packet forwarding between the interfaces on or off.", cmd_inetgateway },
    { 0 },
};

struct ros_module internet_module = {
    .title = "Internet",
    .help = "Internet\t5.68 (24 Sep 2026) ROSGD native, over Linux",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .commands = commands,
    .swi_chunk = 0x41200,
    .swi_thunks = ros_swi_thunks_Internet,
    .swi_names = ros_swi_names_Internet,
    .swi_prefix = "Socket",
};

__attribute__((constructor)) static void count(void)
{
    internet_module.swi_count = ros_swi_count_Internet;
}
