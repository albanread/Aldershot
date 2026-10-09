/* socket.c: the box's BSD sockets, built on lwIP's raw API.
 *
 * The box's Internet module, its resolver (musl's, which works over UDP),
 * AcornHTTP's libcurl and other programs make Linux's socket calls.  Under
 * Linux the kernel answers them; here this file does.  It supports IPv4
 * only: TCP, UDP, ICMP echo (Linux's ping sockets, and raw ICMP sockets),
 * and the NETLINK_ROUTE dumps of links and addresses that musl's
 * getifaddrs asks for.  There are two interfaces, lo (127.0.0.1) and eth0
 * (the virtio-net device), and the ioctls that ask about them answer as
 * Linux's do.
 *
 * When an ICMP error (destination unreachable or time exceeded) arrives
 * about a UDP datagram, this file puts it in the sending socket's error
 * queue, where a program can read it with MSG_ERRQUEUE if IP_RECVERR is
 * set.  On a connected socket the error also becomes the pending error
 * ECONNREFUSED.  Linux does the same, and traceroute relies on it.
 *
 * lwIP calls back when data, a connection or an error arrives.  This file
 * keeps what lwIP delivers with the socket until the box reads it, and
 * wakes any thread that is waiting on the socket or polling it.  A call
 * that must wait does so in the same way as the HAL's other calls
 * (thread_block_restart): its thread sleeps on the socket and makes the
 * call again when woken.  This repeats until the HAL can answer the call
 * or until its timeout (SO_RCVTIMEO or SO_SNDTIMEO) has passed. */
#include "hal.h"

#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/ip4.h"
#include "lwip/inet_chksum.h"
#include "lwip/netif.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"

struct netif *net_iface(int i);

#define EINTR 4
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define EPIPE 32
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115

#define AF_INET 2
#define AF_NETLINK 16
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 04000
#define MSG_OOB 1
#define MSG_PEEK 2
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_ERRQUEUE 0x2000
#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16

#define SOCKS 128
#define DGRAMS 64
#define ACCEPTS 32

enum { K_TCP = 1, K_UDP, K_ICMP, K_RAW, K_NETLINK };

static struct sock {
    int refs, kind, nonblock, async;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    struct raw_pcb *raw;
    int err;                                    /* pending Linux error, as SO_ERROR returns */
    int connecting, connected, waiting, listening, rx_eof, shut_wr, reset;
    struct pbuf *rx;                            /* TCP: data received but not yet read */
    struct { struct pbuf *p; ip_addr_t addr; u16_t port; } dq[DGRAMS];
    int dq_head, dq_n;
    int acq[ACCEPTS], nacq;                     /* incoming connections, sockets made for them */
    int listener;                               /* for such a socket, its listener until accept() */
    int reuse, keepalive, broadcast, nodelay, oobinline, linger_on, linger_s;
    int rcvbuf, sndbuf, rcvlowat, sndlowat, ttl, tos, recverr, recvttl;
    uint64_t rcvtimeo, sndtimeo;                /* in counter ticks; 0 means no timeout */
    uint16_t icmp_id;
    struct { uint8_t type, code; uint32_t from, to; uint16_t port; } eq[8];    /* ICMP errors */
    int eq_n;
    uint8_t *nl;                                /* netlink: the reply waiting to be read */
    uint32_t nl_len, nl_pos;
} socks[SOCKS];

static uint8_t bounce[65536];

static uint64_t key(struct sock *s) { return (uint64_t)s; }

static void wake(struct sock *s)
{
    futex_wake(key(s), 1L << 30);
    futex_wake(KEY_POLL, 1L << 30);
}

static int sock_new(int kind)
{
    for (int i = 0; i < SOCKS; i++)
        if (!socks[i].refs && !socks[i].kind) {
            struct sock *s = &socks[i];
            memset(s, 0, sizeof *s);
            s->refs = 1;
            s->kind = kind;
            s->listener = -1;
            s->rcvbuf = s->sndbuf = 131072;
            s->rcvlowat = s->sndlowat = 1;
            s->ttl = 64;
            return i;
        }
    return -EMFILE;
}

static struct sock *S(int i)
{
    return i >= 0 && i < SOCKS && socks[i].kind ? &socks[i] : 0;
}

static int lwip_errno(err_t e)
{
    switch (e) {
    case ERR_OK: return 0;
    case ERR_MEM: return ENOMEM;
    case ERR_BUF: return ENOBUFS;
    case ERR_TIMEOUT: return ETIMEDOUT;
    case ERR_RTE: return ENETUNREACH;
    case ERR_INPROGRESS: return EINPROGRESS;
    case ERR_WOULDBLOCK: return EAGAIN;
    case ERR_USE: return EADDRINUSE;
    case ERR_ALREADY: return EALREADY;
    case ERR_ISCONN: return EISCONN;
    case ERR_CONN: return ENOTCONN;
    case ERR_IF: return ENETDOWN;
    case ERR_ABRT: return ECONNABORTED;
    case ERR_RST: return ECONNRESET;
    case ERR_CLSD: return ENOTCONN;
    default: return EINVAL;
    }
}

/* Wait for the socket to change state or for the timeout to pass.  Returns
 * -EAGAIN if the call may not wait (non-blocking) or the timeout has passed. */
static long wait_for(struct frame *fr, struct sock *s, uint64_t timeo, int nonblock)
{
    if (nonblock || !fr)
        return -EAGAIN;
    struct thread *t = thread_current();
    uint64_t now = counter_read();
    if (!t->sock_deadline) {
        t->sock_deadline = timeo ? now + timeo : ~0UL;
    } else if (t->sock_deadline != ~0UL && (int64_t)(now - t->sock_deadline) >= 0) {
        t->sock_deadline = 0;
        return -EAGAIN;
    }
    thread_block_restart(fr, key(s), t->sock_deadline == ~0UL ? 0 : t->sock_deadline);
    return SWITCHED;
}

/* ---- Addresses ----------------------------------------------------------- */

static long get_addr(uint64_t va, uint64_t len, ip_addr_t *ip, u16_t *port)
{
    uint8_t a[16];
    if (len < 16)
        return -EINVAL;
    if (copy_from_box(a, va, 16))
        return -EFAULT;
    if ((a[0] | a[1] << 8) != AF_INET)
        return -EAFNOSUPPORT;
    *port = (u16_t)(a[2] << 8 | a[3]);
    uint32_t be;
    memcpy(&be, a + 4, 4);
    ip_addr_set_ip4_u32(ip, be);
    return 0;
}

/* Write a sockaddr_in into the box at va.  lenp points to its length; this
 * function reads the length on entry and updates it on exit. */
static long put_addr(uint64_t va, uint64_t lenp, const ip_addr_t *ip, u16_t port)
{
    if (!va || !lenp)
        return 0;
    uint32_t len;
    if (copy_from_box(&len, lenp, 4))
        return -EFAULT;
    uint8_t a[16];
    memset(a, 0, sizeof a);
    a[0] = AF_INET;
    a[2] = (uint8_t)(port >> 8), a[3] = (uint8_t)port;
    uint32_t be = ip ? ip4_addr_get_u32(ip_2_ip4(ip)) : 0;
    memcpy(a + 4, &be, 4);
    if (copy_to_box(va, a, len < 16 ? len : 16))
        return -EFAULT;
    len = 16;
    return copy_to_box(lenp, &len, 4) ? -EFAULT : 0;
}

/* ---- Callbacks from lwIP ----------------------------------------------- */

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct sock *s = arg;
    (void)err;
    if (!s) {                                   /* socket closed, so nobody will read it */
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p)
        s->rx_eof = 1;
    else if (s->rx)
        pbuf_cat(s->rx, p);
    else
        s->rx = p;
    wake(s);
    if (s->listener >= 0)
        wake(&socks[s->listener]);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb, (void)len;
    if (arg)
        wake(arg);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    struct sock *s = arg;
    if (!s)
        return;
    s->tcp = 0;                                 /* lwIP has already freed the PCB */
    s->err = s->connecting ? ECONNREFUSED : lwip_errno(err);
    s->reset = 1;
    s->rx_eof = 1;
    s->connecting = s->connected = 0;
    wake(s);
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct sock *s = arg;
    (void)pcb;
    if (s) {
        s->connecting = 0;
        if (err == ERR_OK)
            s->connected = 1;
        else
            s->err = lwip_errno(err);
        wake(s);
    }
    return ERR_OK;
}

static void tcp_hooks(struct sock *s, struct tcp_pcb *pcb)
{
    tcp_arg(pcb, s);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_sent(pcb, tcp_sent_cb);
    tcp_err(pcb, tcp_err_cb);
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct sock *l = arg;
    if (!l || err != ERR_OK || !pcb || l->nacq == ACCEPTS)
        return ERR_MEM;                         /* lwIP then aborts the connection */
    int i = sock_new(K_TCP);
    if (i < 0)
        return ERR_MEM;
    struct sock *s = &socks[i];
    s->refs = 0;                                /* owned by nobody until accept() */
    s->tcp = pcb;
    s->connected = 1;
    s->listener = (int)(l - socks);
    s->nodelay = l->nodelay, s->keepalive = l->keepalive;
    tcp_hooks(s, pcb);
    l->acq[l->nacq++] = i;
    wake(l);
    return ERR_OK;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct sock *s = arg;
    (void)pcb;
    if (!s || s->dq_n == DGRAMS) {
        pbuf_free(p);
        return;
    }
    int k = (s->dq_head + s->dq_n++) % DGRAMS;
    s->dq[k].p = p;
    ip_addr_copy(s->dq[k].addr, *addr);
    s->dq[k].port = port;
    wake(s);
}

/* Deliver ICMP to a ping socket or a raw socket.  A ping socket receives
 * only its own echo replies, with the IP header removed.  A raw socket
 * receives every ICMP packet with the IP header kept, as Linux gives it. */
static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    struct sock *s = arg;
    (void)pcb;
    if (!s || s->dq_n == DGRAMS)
        return 0;
    if (s->kind == K_ICMP) {
        if (p->tot_len < IP_HLEN + 8)
            return 0;
        u8_t hl = (u8_t)((((const u8_t *)p->payload)[0] & 15) * 4), h[8];
        if (pbuf_copy_partial(p, h, 8, hl) != 8 || h[0] != ICMP_ER || (u16_t)(h[4] << 8 | h[5]) != s->icmp_id)
            return 0;
        struct pbuf *q = pbuf_alloc(PBUF_RAW, (u16_t)(p->tot_len - hl), PBUF_RAM);
        if (!q)
            return 0;
        pbuf_copy_partial(p, q->payload, q->len, hl);
        pbuf_free(p);
        p = q;
    }
    else {                                      /* raw: copy it, so lwIP sees it too */
        struct pbuf *q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        if (!q)
            return 0;
        p = q;
    }
    int k = (s->dq_head + s->dq_n++) % DGRAMS;
    s->dq[k].p = p;
    ip_addr_copy(s->dq[k].addr, *addr);
    s->dq[k].port = 0;
    wake(s);
    return s->kind == K_ICMP;                   /* consume an echo reply; pass anything else on */
}

/* This sees every ICMP error the stack receives.  It passes an error about
 * a UDP datagram to the socket that sent the datagram. */
static u8_t icmp_err_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)arg, (void)pcb;
    u8_t b[8 + 20 + 8 + 40];
    u16_t hl = (u16_t)((((const u8_t *)p->payload)[0] & 15) * 4);
    u16_t n = pbuf_copy_partial(p, b, sizeof b, hl);
    if (n < 8 + 20 + 8 || (b[0] != ICMP_DUR && b[0] != ICMP_TE))
        return 0;
    const u8_t *ip = b + 8;
    u16_t ihl = (u16_t)((ip[0] & 15) * 4);
    if (ip[9] != 17 || n < 8 + ihl + 8)
        return 0;
    const u8_t *udp = ip + ihl;
    u16_t sport = (u16_t)(udp[0] << 8 | udp[1]), dport = (u16_t)(udp[2] << 8 | udp[3]);
    uint32_t to;
    memcpy(&to, ip + 16, 4);
    for (int i = 0; i < SOCKS; i++) {
        struct sock *s = &socks[i];
        if (s->kind != K_UDP || !s->udp || s->udp->local_port != sport)
            continue;
        if (s->recverr && s->eq_n < 8) {
            int k = s->eq_n++;
            s->eq[k].type = b[0], s->eq[k].code = b[1];
            s->eq[k].from = ip4_addr_get_u32(ip_2_ip4(addr));
            s->eq[k].to = to, s->eq[k].port = dport;
        }
        if (s->connected && b[0] == ICMP_DUR)
            s->err = b[1] == 3 ? ECONNREFUSED : EHOSTUNREACH;
        wake(s);
        break;
    }
    return 0;
}

static void icmp_watch(void)
{
    static struct raw_pcb *w;
    if (!w && (w = raw_new(IP_PROTO_ICMP)))
        raw_recv(w, icmp_err_cb, 0);
}

/* ---- Creating and closing sockets -------------------------------------- */

long sock_socket(long domain, long type, long proto)
{
    int base = (int)(type & 0xF);
    int nonblock = (type & SOCK_NONBLOCK) != 0;
    int kind;
    if (domain == AF_NETLINK) {
        if (proto != 0)                         /* only NETLINK_ROUTE is supported */
            return -EPROTONOSUPPORT;
        kind = K_NETLINK;
    } else if (domain != AF_INET) {
        return -EAFNOSUPPORT;
    } else if (base == SOCK_STREAM && (proto == 0 || proto == 6)) {
        kind = K_TCP;
    } else if (base == SOCK_DGRAM && (proto == 0 || proto == 17)) {
        kind = K_UDP;
    } else if ((base == SOCK_DGRAM || base == SOCK_RAW) && proto == 1) {
        kind = base == SOCK_DGRAM ? K_ICMP : K_RAW;
    } else {
        return -EPROTONOSUPPORT;
    }
    int i = sock_new(kind);
    if (i < 0)
        return i;
    struct sock *s = &socks[i];
    s->nonblock = nonblock;
    if (kind == K_TCP) {
        if (!(s->tcp = tcp_new()))
            goto nomem;
        tcp_hooks(s, s->tcp);
    } else if (kind == K_UDP) {
        icmp_watch();
        if (!(s->udp = udp_new()))
            goto nomem;
        udp_recv(s->udp, udp_recv_cb, s);
    } else if (kind == K_ICMP || kind == K_RAW) {
        if (!(s->raw = raw_new(IP_PROTO_ICMP)))
            goto nomem;
        raw_recv(s->raw, raw_recv_cb, s);
        s->icmp_id = (u16_t)(hal_random() | 1);
    }
    return i;
nomem:
    s->kind = 0, s->refs = 0;
    return -ENOBUFS;
}

void sock_hold(int i)
{
    if (S(i))
        socks[i].refs++;
}

static void free_rx(struct sock *s)
{
    if (s->rx)
        pbuf_free(s->rx);
    s->rx = 0;
    for (; s->dq_n; s->dq_n--, s->dq_head = (s->dq_head + 1) % DGRAMS)
        pbuf_free(s->dq[s->dq_head].p);
}

static void end(struct sock *s)
{
    if (s->tcp) {
        struct tcp_pcb *pcb = s->tcp;
        tcp_arg(pcb, 0);
        if (pcb->state == LISTEN) {
            tcp_close(pcb);
        } else {
            tcp_recv(pcb, tcp_recv_cb);         /* with arg 0, incoming data is dropped */
            tcp_sent(pcb, 0);
            tcp_err(pcb, 0);
            if ((s->linger_on && !s->linger_s) || s->rx || tcp_close(pcb) != ERR_OK)
                tcp_abort(pcb);                 /* unread data: send a reset, as Linux does */
        }
    }
    if (s->udp)
        udp_remove(s->udp);
    if (s->raw)
        raw_remove(s->raw);
    free_rx(s);
    for (int k = 0; k < s->nacq; k++) {         /* close connections never accepted */
        struct sock *a = &socks[s->acq[k]];
        end(a);
    }
    s->kind = 0;
    s->refs = 0;
    net_poll();
}

void sock_close(int i)
{
    struct sock *s = S(i);
    if (s && --s->refs <= 0)
        end(s);
}

int sock_nonblock(int i, int set)
{
    struct sock *s = S(i);
    if (!s)
        return 0;
    if (set >= 0)
        s->nonblock = set;
    return s->nonblock;
}

static uint32_t rx_ready(const struct sock *s)
{
    if (s->kind == K_TCP)
        return s->rx ? s->rx->tot_len : 0;
    if (s->kind == K_NETLINK)
        return s->nl_len - s->nl_pos;
    return s->dq_n ? s->dq[s->dq_head].p->tot_len : 0;
}

unsigned sock_poll(int i)
{
    struct sock *s = S(i);
    if (!s)
        return POLLERR;
    unsigned r = 0;
    if (s->kind == K_TCP) {
        if (s->listening)
            return s->nacq ? POLLIN : 0;
        if (s->rx || s->rx_eof)
            r |= POLLIN;
        if (s->connected && s->tcp && !s->shut_wr && tcp_sndbuf(s->tcp) > 0)
            r |= POLLOUT;
        if (s->err)
            r |= POLLERR | POLLOUT;
        if (s->reset || (s->rx_eof && s->shut_wr))
            r |= POLLHUP;
        if (!s->connected && !s->connecting && !s->rx_eof && !s->err)
            r |= POLLOUT | POLLHUP;             /* never connected: Linux reports this */
    } else {
        r = POLLOUT;
        if (s->dq_n || s->nl_pos < s->nl_len)
            r |= POLLIN;
        if (s->eq_n || s->err)
            r |= POLLERR;
    }
    return r;
}

/* ---- Binding, listening and connecting --------------------------------- */

long sock_bind(int i, uint64_t va, uint64_t len)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    if (s->kind == K_NETLINK)
        return 0;
    ip_addr_t ip;
    u16_t port;
    long e = get_addr(va, len, &ip, &port);
    if (e)
        return e;
    if (!ip_addr_isany(&ip) && !ip_addr_isloopback(&ip)) {
        int mine = 0;
        for (int k = 0; k < 2; k++) {
            struct netif *n = net_iface(k);
            mine |= n && ip_addr_cmp(&ip, netif_ip_addr4(n));
        }
        if (!mine)
            return -EADDRNOTAVAIL;
    }
    err_t r = ERR_VAL;
    if (s->tcp) {
        if (s->reuse)
            ip_set_option(s->tcp, SOF_REUSEADDR);
        r = tcp_bind(s->tcp, &ip, port);
    } else if (s->udp) {
        if (s->reuse)
            ip_set_option(s->udp, SOF_REUSEADDR);
        r = udp_bind(s->udp, &ip, port);
    } else if (s->raw) {
        r = raw_bind(s->raw, &ip);
    }
    return r == ERR_OK ? 0 : -lwip_errno(r);
}

long sock_listen(int i, long backlog)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    if (s->kind != K_TCP)
        return -EOPNOTSUPP;
    if (s->listening)
        return 0;
    if (!s->tcp)
        return -EINVAL;
    if (!s->tcp->local_port && tcp_bind(s->tcp, IP_ADDR_ANY, 0) != ERR_OK)
        return -EADDRINUSE;
    if (backlog < 1 || backlog > 255)
        backlog = backlog < 1 ? 1 : 255;
    struct tcp_pcb *l = tcp_listen_with_backlog(s->tcp, (u8_t)backlog);
    if (!l)
        return -ENOBUFS;
    s->tcp = l;
    s->listening = 1;
    tcp_arg(l, s);
    tcp_accept(l, tcp_accept_cb);
    return 0;
}

long sock_accept(struct frame *fr, int i, uint64_t va, uint64_t lenp)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    if (!s->listening)
        return -EINVAL;
    net_poll();
    if (!s->nacq)
        return wait_for(fr, s, s->rcvtimeo, s->nonblock);
    int a = s->acq[0];
    memmove(s->acq, s->acq + 1, (size_t)--s->nacq * sizeof s->acq[0]);
    struct sock *n = &socks[a];
    n->refs = 1;
    n->listener = -1;
    if (n->tcp) {
        tcp_backlog_accepted(n->tcp);
        long e = put_addr(va, lenp, &n->tcp->remote_ip, n->tcp->remote_port);
        if (e) {
            end(n);
            return e;
        }
    }
    return a;
}

long sock_connect(struct frame *fr, int i, uint64_t va, uint64_t len)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    ip_addr_t ip;
    u16_t port;
    long e;
    if (s->kind == K_UDP) {
        uint16_t fam = 0;
        if (len >= 2 && !copy_from_box(&fam, va, 2) && fam == 0) {     /* AF_UNSPEC: disconnect */
            udp_disconnect(s->udp);
            s->connected = 0;
            return 0;
        }
        if ((e = get_addr(va, len, &ip, &port)))
            return e;
        err_t r = udp_connect(s->udp, &ip, port);
        s->connected = r == ERR_OK;
        return r == ERR_OK ? 0 : -lwip_errno(r);
    }
    if (s->kind == K_ICMP || s->kind == K_RAW) {
        if ((e = get_addr(va, len, &ip, &port)))
            return e;
        raw_connect(s->raw, &ip);
        s->connected = 1;
        return 0;
    }
    if (s->kind != K_TCP)
        return -EOPNOTSUPP;
    if (s->waiting) {                           /* the same call, restarted after waiting */
        net_poll();
        if (s->connected) {
            s->waiting = 0;
            return 0;
        }
        if (s->err) {
            s->waiting = 0;
            e = s->err;
            s->err = 0;
            return -e;
        }
        return wait_for(fr, s, s->sndtimeo, 0);
    }
    if (s->listening)
        return -EINVAL;
    if (s->connected)
        return -EISCONN;
    if (s->connecting)
        return -EALREADY;
    if (s->err && !s->tcp) {
        e = s->err;
        s->err = 0;
        return -e;
    }
    if ((e = get_addr(va, len, &ip, &port)))
        return e;
    if (!s->tcp)
        return -EINVAL;
    s->connecting = 1;
    s->err = 0;
    err_t r = tcp_connect(s->tcp, &ip, port, tcp_connected_cb);
    if (r != ERR_OK) {
        s->connecting = 0;
        return -lwip_errno(r);
    }
    net_poll();                                 /* the loopback interface answers at once */
    if (s->connected)
        return 0;
    if (s->err && !s->connecting) {
        e = s->err;
        s->err = 0;
        return -e;
    }
    if (s->nonblock)
        return -EINPROGRESS;
    s->waiting = 1;
    return wait_for(fr, s, s->sndtimeo, 0);
}

long sock_name(int i, uint64_t va, uint64_t lenp, int peer)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    if (s->kind == K_NETLINK) {
        uint8_t a[12] = { AF_NETLINK };
        uint32_t len = 12;
        return copy_to_box(va, a, 12) || copy_to_box(lenp, &len, 4) ? -EFAULT : 0;
    }
    if (s->tcp) {
        if (peer && !s->connected)
            return -ENOTCONN;
        return peer ? put_addr(va, lenp, &s->tcp->remote_ip, s->tcp->remote_port)
                    : put_addr(va, lenp, &s->tcp->local_ip, s->tcp->local_port);
    }
    if (s->udp) {
        if (peer && !s->connected)
            return -ENOTCONN;
        return peer ? put_addr(va, lenp, &s->udp->remote_ip, s->udp->remote_port)
                    : put_addr(va, lenp, &s->udp->local_ip, s->udp->local_port);
    }
    if (s->raw)
        return peer ? (s->connected ? put_addr(va, lenp, &s->raw->remote_ip, 0) : -ENOTCONN)
                    : put_addr(va, lenp, &s->raw->local_ip, s->icmp_id);
    if (peer)
        return -ENOTCONN;
    return put_addr(va, lenp, 0, 0);
}

long sock_shutdown(int i, long how)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    if (how < 0 || how > 2)
        return -EINVAL;
    if (s->kind != K_TCP)
        return s->connected ? 0 : -ENOTCONN;
    if (!s->connected && !s->rx_eof)
        return -ENOTCONN;
    if (how != 1) {
        s->rx_eof = 1;
        if (s->rx && s->tcp)
            tcp_recved(s->tcp, s->rx->tot_len);
        free_rx(s);
    }
    if (how != 0 && !s->shut_wr) {
        s->shut_wr = 1;
        if (s->tcp)
            tcp_shutdown(s->tcp, 0, 1);
    }
    net_poll();
    wake(s);
    return 0;
}

/* ---- Netlink: the dumps of links and addresses ------------------------ */

static uint32_t nl_put(uint8_t *b, uint32_t at, uint16_t type, const void *d, uint16_t len)
{
    uint16_t l = (uint16_t)(4 + len);
    memcpy(b + at, &l, 2);
    memcpy(b + at + 2, &type, 2);
    memcpy(b + at + 4, d, len);
    memset(b + at + 4 + len, 0, (size_t)((l + 3) & ~3) - l);
    return at + ((l + 3u) & ~3u);
}

static uint32_t nl_head(uint8_t *b, uint32_t at, uint16_t type, uint32_t seq)
{
    uint32_t h[4] = { 0, (uint32_t)type | 2u << 16, seq, 0 };      /* NLM_F_MULTI */
    memcpy(b + at, h, 16);
    return at + 16;
}

static void nl_end(uint8_t *b, uint32_t start, uint32_t at)
{
    uint32_t len = at - start;
    memcpy(b + start, &len, 4);
}

static const char *if_name(int k) { return k ? "eth0" : "lo"; }

static long nl_request(struct sock *s, uint64_t buf, uint64_t n)
{
    uint32_t h[4];
    if (n < 16 || copy_from_box(h, buf, 16))
        return -EINVAL;
    uint16_t type = (uint16_t)h[1];
    uint32_t seq = h[2];
    if (!s->nl) {
        uint64_t pa = page_alloc();
        if (!pa)
            return -ENOMEM;
        s->nl = pa_to_va(pa);
    }
    uint8_t *b = s->nl;
    uint32_t at = 0;
    for (int k = 0; k < 2; k++) {
        struct netif *nf = net_iface(k);
        if (!nf)
            continue;
        uint32_t start = at;
        /* Flags: UP and RUNNING, with BROADCAST and MULTICAST (eth0) or LOOPBACK (lo) */
        uint32_t flags = 1 | 0x40 | (k ? 2 | 0x1000 : 8);
        if (type == 18) {                       /* RTM_GETLINK is answered with RTM_NEWLINK */
            at = nl_head(b, at, 16, seq);
            uint8_t ifi[16] = { 0 };
            uint16_t arphrd = k ? 1 : 772;
            int32_t index = k + 1;
            memcpy(ifi + 2, &arphrd, 2);
            memcpy(ifi + 4, &index, 4);
            memcpy(ifi + 8, &flags, 4);
            memcpy(b + at, ifi, 16), at += 16;
            at = nl_put(b, at, 3, if_name(k), (uint16_t)(strlen(if_name(k)) + 1));     /* IFLA_IFNAME */
            uint8_t hw[6] = { 0 }, bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            if (k)
                memcpy(hw, nf->hwaddr, 6);
            at = nl_put(b, at, 1, hw, 6);                               /* IFLA_ADDRESS */
            at = nl_put(b, at, 2, k ? bc : hw, 6);                      /* IFLA_BROADCAST */
            uint32_t mtu = k ? nf->mtu : 65536;
            at = nl_put(b, at, 4, &mtu, 4);                             /* IFLA_MTU */
        } else if (type == 22) {                /* RTM_GETADDR is answered with RTM_NEWADDR */
            uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(nf)), mask = ip4_addr_get_u32(netif_ip4_netmask(nf));
            if (!ip)
                continue;
            at = nl_head(b, at, 20, seq);
            uint8_t prefix = 0;
            for (uint32_t m = mask; m; m &= m - 1)
                prefix++;
            uint8_t ifa[8] = { AF_INET, prefix, 0, (uint8_t)(k ? 0 : 254) };
            uint32_t index = (uint32_t)k + 1;
            memcpy(ifa + 4, &index, 4);
            memcpy(b + at, ifa, 8), at += 8;
            at = nl_put(b, at, 1, &ip, 4);                              /* IFA_ADDRESS */
            at = nl_put(b, at, 2, &ip, 4);                              /* IFA_LOCAL */
            if (k) {
                uint32_t bcast = ip | ~mask;
                at = nl_put(b, at, 4, &bcast, 4);                       /* IFA_BROADCAST */
            }
            at = nl_put(b, at, 3, if_name(k), (uint16_t)(strlen(if_name(k)) + 1));     /* IFA_LABEL */
        } else {
            break;
        }
        nl_end(b, start, at);
    }
    uint32_t start = at;
    at = nl_head(b, at, 3, seq);                /* NLMSG_DONE */
    memset(b + at, 0, 4), at += 4;
    nl_end(b, start, at);
    s->nl_len = at, s->nl_pos = 0;
    return (long)n;
}

static long nl_reply(struct sock *s, uint64_t buf, uint64_t n, long flags)
{
    uint32_t left = s->nl_len - s->nl_pos;
    if (!left)
        return -EAGAIN;
    uint32_t c = left < n ? left : (uint32_t)n;
    if (copy_to_box(buf, s->nl + s->nl_pos, c))
        return -EFAULT;
    if (!(flags & MSG_PEEK))
        s->nl_pos += c;
    return c;
}

/* ---- Sending and receiving data ------------------------------------------ */

long sock_send(struct frame *fr, int i, uint64_t buf, uint64_t n, long flags, uint64_t va, uint64_t alen)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    int nb = s->nonblock || (flags & MSG_DONTWAIT);
    long e;
    if (s->kind == K_NETLINK)
        return nl_request(s, buf, n);
    if (s->kind == K_TCP) {
        net_poll();
        if (s->err) {
            e = s->err;
            s->err = 0;
            return -e;
        }
        if (!s->connected || !s->tcp)
            return s->reset || s->shut_wr ? -EPIPE : -ENOTCONN;
        if (s->shut_wr)
            return -EPIPE;
        if (!n)
            return 0;
        uint32_t room = tcp_sndbuf(s->tcp);
        if (tcp_sndqueuelen(s->tcp) >= TCP_SND_QUEUELEN - 4)
            room = 0;
        if (!room)
            return wait_for(fr, s, s->sndtimeo, nb);
        uint32_t c = n < room ? (uint32_t)n : room;
        if (c > sizeof bounce)
            c = sizeof bounce;
        if (copy_from_box(bounce, buf, c))
            return -EFAULT;
        err_t r = tcp_write(s->tcp, bounce, (u16_t)(c > 0xFFFF ? 0xFFFF : c), TCP_WRITE_FLAG_COPY);
        if (c > 0xFFFF)
            c = 0xFFFF;
        if (r == ERR_MEM)
            return wait_for(fr, s, s->sndtimeo, nb);
        if (r != ERR_OK)
            return -lwip_errno(r);
        tcp_output(s->tcp);
        net_poll();
        return c;
    }
    /* Datagram sockets.  Return a pending ICMP error first, once only. */
    if (s->err) {
        e = s->err;
        s->err = 0;
        return -e;
    }
    ip_addr_t ip;
    u16_t port = 0;
    if (va) {
        if ((e = get_addr(va, alen, &ip, &port)))
            return e;
    } else if (!s->connected) {
        return -EDESTADDRREQ;
    }
    if (n > 65507)
        return -EMSGSIZE;
    struct pbuf *p = pbuf_alloc(s->kind == K_UDP ? PBUF_TRANSPORT : PBUF_IP, (u16_t)n, PBUF_RAM);
    if (!p)
        return -ENOBUFS;
    if (copy_from_box(p->payload, buf, n)) {
        pbuf_free(p);
        return -EFAULT;
    }
    err_t r;
    if (s->kind == K_UDP) {
        if (s->broadcast)
            ip_set_option(s->udp, SOF_BROADCAST);
        s->udp->ttl = (u8_t)s->ttl;
        r = va ? udp_sendto(s->udp, p, &ip, port) : udp_send(s->udp, p);
    } else {
        if (s->kind == K_ICMP && n >= 8) {      /* ping socket: set its id and the checksum */
            u8_t *m = p->payload;
            if (m[0] != ICMP_ECHO) {
                pbuf_free(p);
                return -EINVAL;
            }
            m[4] = (u8_t)(s->icmp_id >> 8), m[5] = (u8_t)s->icmp_id;
            m[2] = m[3] = 0;
            u16_t sum = inet_chksum(m, (u16_t)n);
            memcpy(m + 2, &sum, 2);
        }
        s->raw->ttl = (u8_t)s->ttl;
        r = va ? raw_sendto(s->raw, p, &ip) : raw_send(s->raw, p);
    }
    pbuf_free(p);
    net_poll();
    if (r == ERR_RTE)
        return -ENETUNREACH;
    return r == ERR_OK ? (long)n : -lwip_errno(r);
}

long sock_recv(struct frame *fr, int i, uint64_t buf, uint64_t n, long flags, uint64_t va, uint64_t alenp)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    int nb = s->nonblock || (flags & MSG_DONTWAIT);
    net_poll();
    if (s->kind == K_NETLINK) {
        long r = nl_reply(s, buf, n, flags);
        if (r >= 0 && va) {
            uint8_t a[12] = { AF_NETLINK };
            uint32_t len = 12;
            if (copy_to_box(va, a, 12) || copy_to_box(alenp, &len, 4))
                return -EFAULT;
        }
        return r;
    }
    if (s->kind == K_TCP) {
        if (s->listening)
            return -ENOTCONN;
        if (s->rx) {
            uint32_t c = s->rx->tot_len < n ? s->rx->tot_len : (uint32_t)n;
            if (c > sizeof bounce)
                c = sizeof bounce;
            pbuf_copy_partial(s->rx, bounce, (u16_t)c, 0);
            if (copy_to_box(buf, bounce, c))
                return -EFAULT;
            if (!(flags & MSG_PEEK)) {
                s->rx = pbuf_free_header(s->rx, (u16_t)c);
                if (s->tcp)
                    tcp_recved(s->tcp, (u16_t)c);
            }
            if (va)
                put_addr(va, alenp, s->tcp ? &s->tcp->remote_ip : 0, s->tcp ? s->tcp->remote_port : 0);
            return c;
        }
        if (s->err && s->reset) {
            long e = s->err;
            s->err = 0;
            return -e;
        }
        if (s->rx_eof)
            return 0;
        if (!s->connected)
            return -ENOTCONN;
        if (!n)
            return 0;
        return wait_for(fr, s, s->rcvtimeo, nb);
    }
    if (s->err && !s->dq_n) {                   /* ICMP error: returned once, as Linux does */
        long e = s->err;
        s->err = 0;
        return -e;
    }
    if (!s->dq_n)
        return wait_for(fr, s, s->rcvtimeo, nb);
    struct pbuf *p = s->dq[s->dq_head].p;
    uint32_t c = p->tot_len < n ? p->tot_len : (uint32_t)n;
    if (c > sizeof bounce)
        c = sizeof bounce;
    pbuf_copy_partial(p, bounce, (u16_t)c, 0);
    if (copy_to_box(buf, bounce, c))
        return -EFAULT;
    long r = (flags & MSG_TRUNC) ? (long)p->tot_len : (long)c;
    if (va && put_addr(va, alenp, &s->dq[s->dq_head].addr, s->dq[s->dq_head].port))
        return -EFAULT;
    if (!(flags & MSG_PEEK)) {
        pbuf_free(p);
        s->dq_head = (s->dq_head + 1) % DGRAMS;
        s->dq_n--;
    }
    return r;
}

/* sendmsg and recvmsg handle the iovecs one at a time, and use or fill in
 * the address given in the message.  They do not support control messages. */
struct msghdr_box {                             /* musl's layout on AArch64 */
    uint64_t name;
    uint32_t namelen, pad0;
    uint64_t iov;
    int32_t iovlen, pad1;
    uint64_t control;
    uint32_t controllen, pad2;
    int32_t flags, pad3;
};

long sock_sendmsg(struct frame *fr, int i, uint64_t msg, long flags)
{
    struct msghdr_box m;
    if (copy_from_box(&m, msg, sizeof m))
        return -EFAULT;
    /* Send each iovec in turn and return the total sent.  This is correct
     * for a stream; a datagram must be in one iovec. */
    long total = 0;
    for (uint64_t k = 0; k < (uint64_t)(m.iovlen > 0 ? m.iovlen : 0); k++) {
        uint64_t v[2];
        if (copy_from_box(v, m.iov + k * 16, 16))
            return total ? total : -EFAULT;
        if (!v[1])
            continue;
        long r = sock_send(total ? 0 : fr, i, v[0], v[1], flags, m.name, m.namelen);
        if (r == SWITCHED)
            return r;
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < v[1])
            break;
    }
    return total;
}

/* MSG_ERRQUEUE returns the oldest ICMP error as Linux's IP_RECVERR control
 * message: a sock_extended_err followed by the address of the host that
 * sent the error.  It returns no data. */
static long err_queue(struct sock *s, uint64_t msg, const struct msghdr_box *m)
{
    if (!s->eq_n)
        return -EAGAIN;
    uint8_t c[16 + 16 + 16];
    memset(c, 0, sizeof c);
    uint64_t clen = sizeof c;
    int32_t level = 0, type = 11;               /* IPPROTO_IP, IP_RECVERR */
    memcpy(c, &clen, 8), memcpy(c + 8, &level, 4), memcpy(c + 12, &type, 4);
    uint32_t eno = s->eq[0].type == ICMP_DUR && s->eq[0].code == 3 ? ECONNREFUSED : EHOSTUNREACH;
    memcpy(c + 16, &eno, 4);
    c[20] = 2;                                  /* SO_EE_ORIGIN_ICMP */
    c[21] = s->eq[0].type, c[22] = s->eq[0].code;
    c[32] = AF_INET;
    memcpy(c + 36, &s->eq[0].from, 4);
    if (m->control && m->controllen >= sizeof c && copy_to_box(m->control, c, sizeof c))
        return -EFAULT;
    uint32_t cl = m->control && m->controllen >= sizeof c ? (uint32_t)sizeof c : 0, fl = MSG_ERRQUEUE;
    copy_to_box(msg + offsetof(struct msghdr_box, controllen), &cl, 4);
    copy_to_box(msg + offsetof(struct msghdr_box, flags), &fl, 4);
    if (m->name && m->namelen >= 16) {          /* the datagram's original destination */
        uint8_t a[16] = { AF_INET, 0, (uint8_t)(s->eq[0].port >> 8), (uint8_t)s->eq[0].port };
        memcpy(a + 4, &s->eq[0].to, 4);
        uint32_t nl = 16;
        copy_to_box(m->name, a, 16);
        copy_to_box(msg + offsetof(struct msghdr_box, namelen), &nl, 4);
    }
    memmove(s->eq, s->eq + 1, (size_t)--s->eq_n * sizeof s->eq[0]);
    return 0;
}

long sock_recvmsg(struct frame *fr, int i, uint64_t msg, long flags)
{
    struct msghdr_box m;
    if (copy_from_box(&m, msg, sizeof m))
        return -EFAULT;
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    if (flags & MSG_ERRQUEUE) {
        net_poll();
        return err_queue(s, msg, &m);
    }
    long total = 0;
    uint64_t namelenp = msg + offsetof(struct msghdr_box, namelen);
    for (uint64_t k = 0; k < (uint64_t)(m.iovlen > 0 ? m.iovlen : 0); k++) {
        uint64_t v[2];
        if (copy_from_box(v, m.iov + k * 16, 16))
            return total ? total : -EFAULT;
        if (!v[1])
            continue;
        long r = sock_recv(total ? 0 : fr, i, v[0], v[1], flags, total ? 0 : m.name, total ? 0 : namelenp);
        if (r == SWITCHED)
            return r;
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < v[1] || s->kind != K_TCP)
            break;
    }
    uint32_t zero = 0;
    copy_to_box(msg + offsetof(struct msghdr_box, controllen), &zero, 4);
    copy_to_box(msg + offsetof(struct msghdr_box, flags), &zero, 4);
    return total;
}

/* ---- Socket options ------------------------------------------------------ */

static long put_int(uint64_t val, uint64_t lenp, int v)
{
    uint32_t len;
    if (copy_from_box(&len, lenp, 4))
        return -EFAULT;
    if (len < 4)
        return -EINVAL;
    len = 4;
    return copy_to_box(val, &v, 4) || copy_to_box(lenp, &len, 4) ? -EFAULT : 0;
}

static uint64_t tv_ticks(const int64_t tv[2])
{
    uint64_t f = counter_freq();
    return (uint64_t)tv[0] * f + (uint64_t)tv[1] * f / 1000000;
}

long sock_setopt(int i, long level, long name, uint64_t val, uint64_t len)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    int v = 0;
    int64_t tv[2] = { 0, 0 };
    if ((level == 1 && (name == 20 || name == 21))) {   /* SO_RCVTIMEO, SO_SNDTIMEO */
        if (len < 16 || copy_from_box(tv, val, 16))
            return -EINVAL;
    } else if (level == 1 && name == 13) {              /* SO_LINGER */
        int l[2];
        if (len < 8 || copy_from_box(l, val, 8))
            return -EINVAL;
        s->linger_on = l[0], s->linger_s = l[1];
        return 0;
    } else {
        if (len < 1)
            return -EINVAL;
        uint8_t b[4] = { 0 };
        if (copy_from_box(b, val, len < 4 ? len : 4))
            return -EFAULT;
        v = len >= 4 ? (int)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24) : b[0];
    }
    if (level == 1) {                           /* SOL_SOCKET */
        switch (name) {
        case 2: s->reuse = v; return 0;                         /* SO_REUSEADDR */
        case 15: s->reuse = v; return 0;                        /* SO_REUSEPORT */
        case 6: s->broadcast = v; return 0;                     /* SO_BROADCAST */
        case 7: s->sndbuf = v; return 0;                        /* SO_SNDBUF */
        case 8: s->rcvbuf = v; return 0;                        /* SO_RCVBUF */
        case 10: s->oobinline = v; return 0;                    /* SO_OOBINLINE */
        case 18: s->rcvlowat = v; return 0;                     /* SO_RCVLOWAT */
        case 19: s->sndlowat = v; return 0;                     /* SO_SNDLOWAT */
        case 9:                                                 /* SO_KEEPALIVE */
            s->keepalive = v;
            if (s->tcp) {
                if (v)
                    ip_set_option(s->tcp, SOF_KEEPALIVE);
                else
                    ip_reset_option(s->tcp, SOF_KEEPALIVE);
            }
            return 0;
        case 20: s->rcvtimeo = tv_ticks(tv); return 0;
        case 21: s->sndtimeo = tv_ticks(tv); return 0;
        }
        return -ENOPROTOOPT;
    }
    if (level == 6 && s->kind == K_TCP) {       /* IPPROTO_TCP */
        if (name == 1) {                        /* TCP_NODELAY */
            s->nodelay = v;
            if (s->tcp) {
                if (v)
                    tcp_nagle_disable(s->tcp);
                else
                    tcp_nagle_enable(s->tcp);
            }
            return 0;
        }
        /* TCP_MAXSEG, TCP_KEEPIDLE, TCP_KEEPINTVL and TCP_KEEPCNT are accepted and ignored */
        if (name == 2 || name == 4 || name == 5 || name == 6)
            return 0;
        return -ENOPROTOOPT;
    }
    if (level == 0) {                           /* IPPROTO_IP */
        switch (name) {
        case 2: s->ttl = v & 255; return 0;                     /* IP_TTL */
        case 1: s->tos = v & 255; return 0;                     /* IP_TOS */
        case 11: s->recverr = v; return 0;                      /* IP_RECVERR */
        case 12: s->recvttl = v; return 0;                      /* IP_RECVTTL */
        }
        return -ENOPROTOOPT;
    }
    if (level == 270 && s->kind == K_NETLINK)   /* SOL_NETLINK */
        return 0;
    return -ENOPROTOOPT;
}

long sock_getopt(int i, long level, long name, uint64_t val, uint64_t lenp)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    if (level == 1) {
        switch (name) {
        case 3: return put_int(val, lenp, s->kind == K_TCP ? SOCK_STREAM : s->kind == K_RAW ? SOCK_RAW : SOCK_DGRAM);
        case 4: {                                               /* SO_ERROR, which also clears it */
            int e = s->err;
            s->err = 0;
            return put_int(val, lenp, e);
        }
        case 2: case 15: return put_int(val, lenp, s->reuse);
        case 6: return put_int(val, lenp, s->broadcast);
        case 7: return put_int(val, lenp, s->sndbuf);
        case 8: return put_int(val, lenp, s->rcvbuf);
        case 9: return put_int(val, lenp, s->keepalive);
        case 10: return put_int(val, lenp, s->oobinline);
        case 18: return put_int(val, lenp, s->rcvlowat);
        case 19: return put_int(val, lenp, s->sndlowat);
        case 30: return put_int(val, lenp, s->listening);       /* SO_ACCEPTCONN */
        case 39: return put_int(val, lenp, s->kind == K_TCP ? 6 : s->kind == K_UDP ? 17 : 1);   /* SO_PROTOCOL */
        case 13: {                                              /* SO_LINGER */
            int l[2] = { s->linger_on, s->linger_s };
            uint32_t len = 8;
            return copy_to_box(val, l, 8) || copy_to_box(lenp, &len, 4) ? -EFAULT : 0;
        }
        case 20: case 21: {                                     /* the timeouts, as timevals */
            uint64_t t = name == 20 ? s->rcvtimeo : s->sndtimeo, f = counter_freq();
            int64_t tv[2] = { (int64_t)(t / f), (int64_t)(t % f * 1000000 / f) };
            uint32_t len = 16;
            return copy_to_box(val, tv, 16) || copy_to_box(lenp, &len, 4) ? -EFAULT : 0;
        }
        }
        return -ENOPROTOOPT;
    }
    if (level == 6 && s->kind == K_TCP) {
        if (name == 1)
            return put_int(val, lenp, s->nodelay);
        if (name == 2)
            return put_int(val, lenp, s->tcp ? tcp_mss(s->tcp) : TCP_MSS);
        return -ENOPROTOOPT;
    }
    if (level == 0) {
        switch (name) {
        case 2: return put_int(val, lenp, s->ttl);
        case 1: return put_int(val, lenp, s->tos);
        case 11: return put_int(val, lenp, s->recverr);
        case 12: return put_int(val, lenp, s->recvttl);
        }
    }
    return -ENOPROTOOPT;
}

/* ---- ioctls on the socket and on the interfaces ------------------------- */

static int iface_by_name(const char *n)
{
    for (int k = 0; k < 2; k++)
        if (net_iface(k) && !strcmp(n, if_name(k)))
            return k;
    return -1;
}

static void sin(uint8_t *a, uint32_t be)
{
    memset(a, 0, 16);
    a[0] = AF_INET;
    memcpy(a + 4, &be, 4);
}

long sock_ioctl(int i, uint32_t req, uint64_t arg)
{
    struct sock *s = S(i);
    if (!s)
        return -EBADF;
    int v;
    switch (req) {
    case 0x5421:                                /* FIONBIO */
        if (copy_from_box(&v, arg, 4))
            return -EFAULT;
        s->nonblock = v != 0;
        return 0;
    case 0x5452:                                /* FIOASYNC */
        if (copy_from_box(&v, arg, 4))
            return -EFAULT;
        s->async = v != 0;
        return 0;
    case 0x541B:                                /* FIONREAD */
        net_poll();
        v = (int)rx_ready(s);
        return copy_to_box(arg, &v, 4) ? -EFAULT : 0;
    case 0x8905:                                /* SIOCATMARK: there is never urgent data */
        v = 0;
        return copy_to_box(arg, &v, 4) ? -EFAULT : 0;
    case 0x8912: {                              /* SIOCGIFCONF */
        struct { int32_t len, pad; uint64_t buf; } ifc;
        if (copy_from_box(&ifc, arg, 16))
            return -EFAULT;
        int32_t used = 0;
        for (int k = 0; k < 2; k++) {
            struct netif *nf = net_iface(k);
            if (!nf || !ip4_addr_get_u32(netif_ip4_addr(nf)))
                continue;
            if (ifc.buf) {
                if (used + 40 > ifc.len)
                    break;
                uint8_t r[40];
                memset(r, 0, sizeof r);
                memcpy(r, if_name(k), strlen(if_name(k)));
                sin(r + 16, ip4_addr_get_u32(netif_ip4_addr(nf)));
                if (copy_to_box(ifc.buf + (uint64_t)used, r, 40))
                    return -EFAULT;
            }
            used += 40;
        }
        ifc.len = used;
        return copy_to_box(arg, &ifc, 4) ? -EFAULT : 0;
    }
    case 0x8913: case 0x8915: case 0x8919: case 0x891b: case 0x8921: case 0x8927: case 0x8933: {
        uint8_t r[40];
        if (copy_from_box(r, arg, 40))
            return -EFAULT;
        r[15] = 0;
        int k = iface_by_name((const char *)r);
        if (k < 0)
            return -19;                         /* ENODEV */
        struct netif *nf = net_iface(k);
        uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(nf)), mask = ip4_addr_get_u32(netif_ip4_netmask(nf));
        memset(r + 16, 0, 24);
        switch (req) {
        case 0x8913: {                          /* SIOCGIFFLAGS */
            uint16_t f = (uint16_t)(1 | 0x40 | (k ? 2 | 0x1000 : 8));
            memcpy(r + 16, &f, 2);
            break;
        }
        case 0x8915: sin(r + 16, ip); break;                    /* SIOCGIFADDR */
        case 0x8919: sin(r + 16, k ? (ip | ~mask) : 0); break;  /* SIOCGIFBRDADDR */
        case 0x891b: sin(r + 16, mask); break;                  /* SIOCGIFNETMASK */
        case 0x8921: {                                          /* SIOCGIFMTU */
            int32_t mtu = k ? nf->mtu : 65536;
            memcpy(r + 16, &mtu, 4);
            break;
        }
        case 0x8927:                                            /* SIOCGIFHWADDR */
            r[16] = k ? 1 : 0x04, r[17] = k ? 0 : 0x03;         /* ARPHRD_ETHER, _LOOPBACK */
            if (k)
                memcpy(r + 18, nf->hwaddr, 6);
            break;
        case 0x8933: {                                          /* SIOCGIFINDEX */
            int32_t idx = k + 1;
            memcpy(r + 16, &idx, 4);
            break;
        }
        }
        return copy_to_box(arg, r, 40) ? -EFAULT : 0;
    }
    case 0x8914:                                /* SIOCSIFFLAGS: the interface is already up */
        return 0;
    }
    if ((req & 0xFF00) == 0x8900)
        return -1;                              /* EPERM: the other requests change settings */
    return -ENOTTY;
}

/* ---- /proc/net/tcp and udp ---------------------------------------------- */

/* List every endpoint lwIP has, both the box's sockets and lwIP's own (such
 * as DHCP's port 68), in the format Linux uses.  This table gives Linux's
 * state number for each of lwIP's states, CLOSED to TIME_WAIT in order. */
static const uint8_t linux_state[] = { 7, 0x0A, 2, 3, 1, 4, 5, 8, 0x0B, 9, 6 };

static char *proc_line(char *p, char *end, int sl, const ip_addr_t *l, unsigned lp, const ip_addr_t *r,
                       unsigned rp, unsigned st, uint32_t rxq, int ino)
{
    return p + ksnprintf(p, (size_t)(end - p),
                         "%4d: %08X:%04X %08X:%04X %02X %08X:%08X 00:00000000 00000000     0        0 %d 1 0000000000000000 100 0 0 10 0\n",
                         sl, ip4_addr_get_u32(ip_2_ip4(l)), lp, r ? ip4_addr_get_u32(ip_2_ip4(r)) : 0, rp, st, 0, rxq, ino);
}

int sock_proc_net(char *out, size_t max, int udp)
{
    char *p = out, *end = out + max;
    p += ksnprintf(p, (size_t)(end - p),
                   "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    int sl = 0;
    if (udp) {
        for (struct udp_pcb *u = udp_pcbs; u && end - p > 200; u = u->next) {
            struct sock *s = u->recv_arg;
            int conn = (u->flags & UDP_FLAGS_CONNECTED) != 0;
            p = proc_line(p, end, sl, &u->local_ip, u->local_port, conn ? &u->remote_ip : 0,
                          conn ? u->remote_port : 0, conn ? 1 : 7, s ? rx_ready(s) : 0, 1000 + sl), sl++;
        }
    } else {
        struct tcp_pcb *const *lists[] = { &tcp_active_pcbs, &tcp_tw_pcbs };
        for (struct tcp_pcb_listen *l = tcp_listen_pcbs.listen_pcbs; l && end - p > 200; l = l->next)
            p = proc_line(p, end, sl, &l->local_ip, l->local_port, 0, 0, 0x0A, 0, 1000 + sl), sl++;
        for (int k = 0; k < 2; k++)
            for (struct tcp_pcb *t = *lists[k]; t && end - p > 200; t = t->next) {
                struct sock *s = t->callback_arg;
                p = proc_line(p, end, sl, &t->local_ip, t->local_port, &t->remote_ip, t->remote_port,
                              t->state <= TIME_WAIT ? linux_state[t->state] : 7, s ? rx_ready(s) : 0, 1000 + sl), sl++;
            }
    }
    return (int)(p - out);
}
