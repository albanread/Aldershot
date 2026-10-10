/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_internet.c: the Internet module, over Linux's sockets, against
 * the documented Socket_* interface (modules/internet/README.md).
 *
 * Everything runs over loopback, so it needs no network and runs the same in
 * the hosted build. It covers TCP and UDP in both sockaddr forms,
 * non-blocking I/O, select, options, the msghdr and iovec calls, errors in
 * RISC OS's numbering, and the interfaces. With ip=dhcp on the kernel
 * command line, the checks add that the kernel's DHCP configured an
 * interface and a name server. With rosgd.nettest=a.b.c.d:port, they add
 * that an HTTP server there answers. That traffic leaves the guest (QEMU's
 * user networking puts the host at 10.0.2.2).
 *
 * Structures that the module reads by address (sockaddrs, msghdrs, iovecs,
 * fd_sets) are built in the RMA, where a RISC OS client's would be.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

#define ERR(n) (0x20E00u + (n))
enum { EBADF = 9, EWOULDBLOCK = 35, ECONNREFUSED = 61 };

static uint32_t errnum(const os_error *e)
{
    return e ? e->errnum : 0;
}

/* A sockaddr_in, 4.4BSD's form (bsd44) or 4.3's, for 127.0.0.1:port. */
static void sin_loop(uint8_t *p, uint32_t port, int bsd44)
{
    memset(p, 0, 16);
    p[0] = bsd44 ? 16 : 2;
    p[1] = bsd44 ? 2 : 0;
    p[2] = (uint8_t)(port >> 8), p[3] = (uint8_t)port;
    p[4] = 127, p[7] = 1;
}

static uint32_t port_of(const uint8_t *p)
{
    return (uint32_t)p[2] << 8 | p[3];
}

void ros_selftest_internet(void)
{
    uint8_t *a = ros_rma_alloc(64), *b = ros_rma_alloc(64);
    uint8_t *buf = ros_rma_alloc(256);
    uint32_t *len = ros_rma_alloc(8), *sets = ros_rma_alloc(32), *tv = ros_rma_alloc(8);
    uint32_t *msg = ros_rma_alloc(32), *iov = ros_rma_alloc(32);
    uint32_t n = 0, v = 0;
    os_error *e;

    /* ---- the SWI chunk, as compiled code reaches it ---- */
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, XSocket_Version);
    check(!s.v && s.r[0] == 568, "Internet: Socket_Version through the SWI chunk", "%u",
          s.r[0]);
    xsocket_gettsize(&v);
    check(v == 256, "Internet: Socket_Gettsize -- 256 sockets", "%u", v);

    /* ---- TCP over loopback ---- */
    uint32_t lst = 0, cli = 0, srv = 0;
    e = xsocket_creat(2, 1, 0, &lst);
    sin_loop(a, 0, 1);
    if (!e)
        e = xsocket_bind(lst, a, 16);
    if (!e)
        e = xsocket_listen(lst, 4);
    *len = 64;
    if (!e)
        e = xsocket_getsockname_1(lst, a, len);
    uint32_t port = port_of(a);
    check(!e && *len == 16 && a[0] == 16 && a[1] == 2 && port != 0,
          "Internet: TCP listener bound, Getsockname_1 in 4.4BSD's form",
          "%s, len %u, bytes %u %u, port %u", e ? e->errmess : "ok", *len, a[0], a[1], port);
    *len = 64;
    xsocket_getsockname(lst, b, len);
    check(b[0] == 2 && b[1] == 0 && port_of(b) == port,
          "Internet: Getsockname in 4.3BSD's form -- a 16-bit family", "%u %u", b[0], b[1]);

    e = xsocket_creat(2, 1, 0, &cli);
    sin_loop(b, port, 0);                       /* connect with 4.3's form */
    if (!e)
        e = xsocket_connect(cli, b, 16);
    *len = 64;
    if (!e)
        e = xsocket_accept_1(lst, a, len, &srv);
    check(!e && a[1] == 2 && a[4] == 127 && a[7] == 1,
          "Internet: Connect in 4.3's form, Accept_1 gives the peer in 4.4's",
          "%s", e ? e->errmess : "ok");

    static const char hello[] = "hello, RISC OS";
    e = xsocket_send(cli, (const uint8_t *)hello, sizeof hello, 0, &n);
    memset(buf, 0, 64);
    uint32_t got = 0;
    if (!e)
        e = xsocket_recv(srv, buf, 64, 0, &got);
    check(!e && n == sizeof hello && got == sizeof hello && memcmp(buf, hello, got) == 0,
          "Internet: Send and Recv", "%s, sent %u, got %u", e ? e->errmess : "ok", n, got);
    e = xsocket_write(srv, (const uint8_t *)"back", 4, &n);
    memset(buf, 0, 64);
    if (!e)
        e = xsocket_read(cli, buf, 64, &got);
    check(!e && got == 4 && memcmp(buf, "back", 4) == 0, "Internet: Write and Read", NULL);

    /* non-blocking, as RISC OS's Wimp programs use sockets */
    v = 1;
    e = xsocket_ioctl(srv, 0x8004667Eu, (uint8_t *)&v);           /* FIONBIO */
    os_error *wb = e ? e : xsocket_recv(srv, buf, 64, 0, &got);
    check(errnum(wb) == ERR(EWOULDBLOCK),
          "Internet: FIONBIO -- an empty socket answers EWOULDBLOCK, &20E23",
          "&%X %s", errnum(wb), wb ? wb->errmess : "");
    xsocket_send(cli, (const uint8_t *)"12345", 5, 0, &n);
    memset(sets, 0, 32);
    sets[srv / 32] = 1u << (srv % 32);
    tv[0] = 1, tv[1] = 0;
    e = xsocket_select(srv + 1, sets, NULL, NULL, tv, &n);
    check(!e && n == 1 && (sets[srv / 32] >> (srv % 32) & 1),
          "Internet: Select -- the socket with data is ready", "%s, %u ready",
          e ? e->errmess : "ok", n);
    v = 0;
    e = xsocket_ioctl(srv, 0x4004667Fu, (uint8_t *)&v);           /* FIONREAD */
    check(!e && v == 5, "Internet: FIONREAD -- five bytes waiting", "%u", v);
    e = xsocket_ioctl(srv, 0x0000667Eu, NULL);                    /* FIONBIO, no direction bits, no argument */
    check(errnum(e) == 0x20E0Eu, "Internet: an ioctl that needs its argument and has none -- EFAULT, &20E0E",
          "&%X %s", errnum(e), e ? e->errmess : "");
    xsocket_recv(srv, buf, 64, 0, &got);
    memset(sets, 0, 32);
    sets[srv / 32] = 1u << (srv % 32);
    tv[0] = 0, tv[1] = 0;
    e = xsocket_select(srv + 1, sets, NULL, NULL, tv, &n);
    check(!e && n == 0 && sets[srv / 32] == 0, "Internet: Select -- nothing ready, no wait",
          "%u ready", n);

    /* readv and writev: iovecs of two words */
    uint8_t *p1 = buf, *p2 = buf + 100;
    memcpy(p1, "scatter", 7), memcpy(p2, "gather", 6);
    iov[0] = ros_addr(p1), iov[1] = 7, iov[2] = ros_addr(p2), iov[3] = 6;
    e = xsocket_writev(cli, iov, 2, &n);
    uint8_t *q = buf + 160;
    memset(q, 0, 32);
    iov[0] = ros_addr(q), iov[1] = 4, iov[2] = ros_addr(q + 4), iov[3] = 20;
    xsocket_ioctl(srv, 0x8004667Eu, (uint8_t *)&(uint32_t){ 0 });   /* blocking again */
    if (!e)
        e = xsocket_readv(srv, iov, 2, &got);
    check(!e && n == 13 && got == 13 && memcmp(q, "scattergather", 13) == 0,
          "Internet: Writev and Readv", "%s, %u/%u", e ? e->errmess : "ok", n, got);

    /* options: levels and names are 4.4BSD's */
    v = 1;
    e = xsocket_setsockopt(cli, 0xFFFF, 0x0004, (uint8_t *)&v, 4);       /* SO_REUSEADDR */
    v = 0, *len = 4;
    if (!e)
        e = xsocket_getsockopt(cli, 0xFFFF, 0x0004, (uint8_t *)&v, len);
    check(!e && v != 0, "Internet: Setsockopt and Getsockopt -- SOL_SOCKET &FFFF",
          "%s", e ? e->errmess : "ok");
    v = 0, *len = 4;
    e = xsocket_getsockopt(cli, 0xFFFF, 0x1008, (uint8_t *)&v, len);     /* SO_TYPE */
    check(!e && v == 1, "Internet: SO_TYPE -- a stream", "%u", v);
    tv[0] = 2, tv[1] = 500000;
    e = xsocket_setsockopt(cli, 0xFFFF, 0x1006, (uint8_t *)tv, 8);       /* SO_RCVTIMEO */
    tv[0] = tv[1] = 0, *len = 8;
    if (!e)
        e = xsocket_getsockopt(cli, 0xFFFF, 0x1006, (uint8_t *)tv, len);
    check(!e && *len == 8 && tv[0] == 2 && tv[1] / 1000 == 500,
          "Internet: SO_RCVTIMEO -- a timeval of two 32-bit words", "%u.%06u", tv[0], tv[1]);

    xsocket_shutdown(cli, 1);
    e = xsocket_recv(srv, buf, 64, 0, &got);
    check(!e && got == 0, "Internet: Shutdown -- the peer reads end of file", NULL);
    xsocket_close(cli);
    xsocket_close(srv);
    xsocket_close(lst);

    /* ---- UDP ---- */
    uint32_t u1 = 0, u2 = 0;
    e = xsocket_creat(2, 2, 0, &u1);
    if (!e)
        e = xsocket_creat(2, 2, 0, &u2);
    sin_loop(a, 0, 1);
    if (!e)
        e = xsocket_bind(u2, a, 16);
    *len = 64;
    if (!e)
        e = xsocket_getsockname_1(u2, a, len);
    uint32_t uport = port_of(a);
    sin_loop(b, uport, 1);
    if (!e)
        e = xsocket_sendto(u1, (const uint8_t *)"datagram", 8, 0, b, 16, &n);
    memset(a, 0, 64), memset(buf, 0, 64), *len = 64;
    if (!e)
        e = xsocket_recvfrom(u2, buf, 64, 0, a, len, &got);
    check(!e && got == 8 && memcmp(buf, "datagram", 8) == 0 && a[0] == 2 && a[1] == 0 &&
              a[4] == 127,
          "Internet: UDP Sendto, and Recvfrom's sender in 4.3's form", "%s", e ? e->errmess : "ok");

    /* sendmsg and recvmsg, 4.4's seven-word msghdr */
    memcpy(buf, "message", 7);
    iov[0] = ros_addr(buf), iov[1] = 7;
    uint32_t m[7] = { ros_addr(b), 16, ros_addr(iov), 1, 0, 0, 0 };
    memcpy(msg, m, sizeof m);
    e = xsocket_sendmsg_1(u1, msg, 0, &n);
    memset(q, 0, 32), memset(a, 0, 64);
    iov[0] = ros_addr(q), iov[1] = 32;
    uint32_t r[7] = { ros_addr(a), 64, ros_addr(iov), 1, 0, 0, 0xFFFFFFFFu };
    memcpy(msg, r, sizeof r);
    if (!e)
        e = xsocket_recvmsg_1(u2, msg, 0, &got);
    check(!e && got == 7 && memcmp(q, "message", 7) == 0 && msg[1] == 16 && a[1] == 2 &&
              msg[6] == 0,
          "Internet: Sendmsg_1 and Recvmsg_1 -- msghdr, iovec and sender",
          "%s, %u bytes, namelen %u", e ? e->errmess : "ok", got, msg[1]);
    xsocket_close(u1);
    xsocket_close(u2);

    /* ---- errors, in RISC OS's numbering ---- */
    e = xsocket_close(200);
    check(errnum(e) == ERR(EBADF), "Internet: a socket never opened -- EBADF, &20E09",
          "&%X", errnum(e));
    e = xsocket_creat(2, 1, 0, &cli);
    sin_loop(b, 1, 1);                          /* nothing listens on port 1 */
    os_error *refused = e ? e : xsocket_connect(cli, b, 16);
    check(errnum(refused) == ERR(ECONNREFUSED),
          "Internet: a closed port -- ECONNREFUSED, &20E3D", "&%X %s", errnum(refused),
          refused ? refused->errmess : "");
    if (!e)
        xsocket_close(cli);

    /* ---- interfaces ---- */
    e = xsocket_creat(2, 2, 0, &u1);
    uint8_t *ifs = ros_rma_alloc(32 * 16);
    uint32_t *ifc = ros_rma_alloc(8);
    ifc[0] = 32 * 16, ifc[1] = ros_addr(ifs);
    if (!e)
        e = xsocket_ioctl(u1, 0xC0086960u, (uint8_t *)ifc);        /* SIOCGIFCONF */
    int loop = -1, dhcp = -1;
    for (uint32_t i = 0; !e && i < ifc[0] / 32; i++) {
        const uint8_t *ifr = ifs + 32 * i;
        if (ifr[16 + 4] == 127)
            loop = (int)i;
        else if (ifr[16 + 4] != 0)
            dhcp = (int)i;
    }
    check(!e && loop >= 0, "Internet: SIOCGIFCONF -- the loopback interface, 127.0.0.1",
          "%s, %u bytes", e ? e->errmess : "ok", ifc[0]);
    if (loop >= 0) {
        memcpy(a, ifs + 32 * loop, 32);
        e = xsocket_ioctl(u1, 0xC0206911u, a);                      /* SIOCGIFFLAGS */
        uint32_t flags = a[16] | (uint32_t)a[17] << 8;
        check(!e && (flags & 0x9) == 0x9, "Internet: SIOCGIFFLAGS -- loopback, up",
              "&%X", flags);
    }
    if (ros_cmdline_has("ip=dhcp")) {
        check(dhcp >= 0, "Internet: the kernel's DHCP configured an interface", NULL);
        if (dhcp >= 0) {
            const uint8_t *ip = ifs + 32 * dhcp + 16 + 4;
            ros_console_printf("        %.16s %u.%u.%u.%u\n", (const char *)(ifs + 32 * dhcp),
                               ip[0], ip[1], ip[2], ip[3]);
        }
        char line[128] = "";
        FILE *f = fopen("/etc/resolv.conf", "r");
        int ns = 0;
        while (f && fgets(line, sizeof line, f))
            ns |= strncmp(line, "nameserver", 10) == 0;
        if (f)
            fclose(f);
        check(ns, "Internet: /etc/resolv.conf names the DHCP server's name server", NULL);
    }
    xsocket_close(u1);

    /* ---- out of the guest: an HTTP request, when asked for ---- */
    const char *want = strstr(ros_cmdline(), "rosgd.nettest=");
    unsigned h[4], hport;
    if (want && sscanf(want + 14, "%u.%u.%u.%u:%u", &h[0], &h[1], &h[2], &h[3], &hport) == 5) {
        memset(b, 0, 16);
        b[0] = 16, b[1] = 2, b[2] = (uint8_t)(hport >> 8), b[3] = (uint8_t)hport;
        for (int i = 0; i < 4; i++)
            b[4 + i] = (uint8_t)h[i];
        e = xsocket_creat(2, 1, 0, &cli);
        if (!e)
            e = xsocket_connect(cli, b, 16);
        static const char get[] = "GET / HTTP/1.0\r\n\r\n";
        if (!e)
            e = xsocket_send(cli, (const uint8_t *)get, sizeof get - 1, 0, &n);
        memset(buf, 0, 256);
        got = 0;
        for (uint32_t k = 1; !e && k && got < 12; got += k)
            e = xsocket_recv(cli, buf + got, 255 - got, 0, &k);
        check(!e && memcmp(buf, "HTTP/1.", 7) == 0,
              "Internet: an HTTP server outside the guest answers", "%u.%u.%u.%u:%u %s \"%.12s\"",
              h[0], h[1], h[2], h[3], hport, e ? e->errmess : "", (const char *)buf);
        xsocket_close(cli);
    }

    ros_rma_free(ifc), ros_rma_free(ifs);
    ros_rma_free(iov), ros_rma_free(msg), ros_rma_free(tv), ros_rma_free(sets);
    ros_rma_free(len), ros_rma_free(buf), ros_rma_free(b), ros_rma_free(a);
}
