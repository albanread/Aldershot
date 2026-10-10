/*
 * Copyright (c) 1988, 1989, 1991, 1994, 1995, 1996, 1997, 1998, 1999, 2000
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the University of California,
 * Lawrence Berkeley Laboratory and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's
 * traceroute(8) as ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/traceroute: c.TraceRoute).
 */

/* traceroute.c -- *TraceRoute (InetRes/Sources/traceroute, FreeBSD's
 * traceroute), over Linux.
 *
 * It sends UDP probes to ports from 33434 up, three to a hop (-q), with the
 * TTL rising from -f to -m. Each answer is the ICMP error that Linux queues
 * on the socket (IP_RECVERR), unprivileged: time exceeded from a router on
 * the way, or port unreachable from the host itself. The output is as
 * FreeBSD's traceroute prints it: the header on the error stream (here the
 * screen), then a line a hop, "  %.3f ms" for a reply, " *" for a probe
 * unanswered, and "!H" and the like for other unreachables. -I (ICMP echo
 * probes) and the rest are not carried. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/errqueue.h>
#endif

#include "inetres.h"

/* musl's CMSG_NXTHDR compares a size_t with a long */
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "rosgd/background.h"

#ifdef __linux__
#define PROBE_DATA 1500                 /* the most data a probe carries */

static double now_ms(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}
#endif

os_error *inet_traceroute(const struct inet_args *a)
{
    struct inet_tool t = { "traceroute", 0 };
#ifndef __linux__
    (void)a;
    return inet_fail(&t, 1, "not available on this host: ROSGD's traceroute reads Linux's ICMP errors");
#else
    struct inet_opt o = { 0 };
    long first = 1, maxttl = 64, nprobes = 3, wait = 5, port = 33434;
    int numeric = 0, c;
    while ((c = inet_getopt(&o, a, "f:m:q:w:p:ndDeFISvxg:i:P:s:t:z:")) != -1) {
        switch (c) {
        case 'f':
            first = strtol(o.arg, NULL, 10);
            break;
        case 'm':
            maxttl = strtol(o.arg, NULL, 10);
            break;
        case 'q':
            nprobes = strtol(o.arg, NULL, 10);
            break;
        case 'w':
            wait = strtol(o.arg, NULL, 10);
            break;
        case 'p':
            port = strtol(o.arg, NULL, 10);
            break;
        case 'n':
            numeric = 1;
            break;
        case '?':
            return inet_fail(&t, 1, "illegal option -- %c", a->argv[o.ind - 1][1]);
        case ':':
            return inet_fail(&t, 1, "option requires an argument -- %c", a->argv[o.ind - 1][1]);
        default:
            return inet_fail(&t, 1, "option -%c is not carried by ROSGD's traceroute", c);
        }
    }
    if (first < 1 || first > 255 || maxttl < first || maxttl > 255)
        return inet_fail(&t, 1, "max ttl must be %ld to 255", first);
    if (nprobes < 1 || wait < 1)
        return inet_fail(&t, 1, "nprobes and wait must be positive");
    if (o.ind >= a->argc || o.ind < a->argc - 2)
        return inet_printf("Usage:   traceroute [-n] [-f first_ttl] [-m max_ttl] [-p port] [-q nqueries]\n"
                           "                    [-w waittime] <host> [packetlen]\n");
    const char *target = a->argv[o.ind];
    long packlen = o.ind + 1 < a->argc ? strtol(a->argv[o.ind + 1], NULL, 10) : 40;
    if (packlen < 28)
        packlen = 28;
    if (packlen > 28 + PROBE_DATA)
        return inet_fail(&t, 1, "packet size must be at most %d", 28 + PROBE_DATA);

    struct addrinfo hints = { .ai_family = AF_INET }, *res;
    if (getaddrinfo(target, NULL, &hints, &res))
        return inet_fail(&t, 1, "unknown host %s", target);
    struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);

    int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1;
    if (s < 0)
        return inet_fail(&t, 1, "socket: %s", strerror(errno));
    setsockopt(s, IPPROTO_IP, IP_RECVERR, &on, sizeof on);
    os_error *e = inet_printf("traceroute to %s (%s), %ld hops max, %ld byte packets\n", target,
                              inet_ntoa(to.sin_addr), maxttl, packlen);
    char data[PROBE_DATA] = { 0 };
    int done = 0;
    for (long ttl = first; !e && ttl <= maxttl && !done; ttl++) {
        e = inet_printf("%2ld ", ttl);
        uint32_t last = 0;
        int got = 0;
        for (long probe = 0; !e && probe < nprobes; probe++) {
            if (inet_escape()) {
                close(s);
                return inet_printf("\n");
            }
            int ittl = (int)ttl;
            setsockopt(s, IPPROTO_IP, IP_TTL, &ittl, sizeof ittl);
            to.sin_port = htons((uint16_t)(port + (ttl - 1) * nprobes + probe));
            double t1 = now_ms();
            sendto(s, data, (size_t)packlen - 28, 0, (struct sockaddr *)&to, sizeof to);
            int answered = 0;
            while (!answered) {
                double left = t1 + wait * 1000.0 - now_ms();
                if (left <= 0)
                    break;
                os_error *slept;
                int r = ros_sleep_fd(s, POLLERR | POLLIN, left > 100 ? 100 : (unsigned)left, &slept);
                if (r < 0 || inet_escape()) {           /* a sleep in a task window: desktop runs */
                    close(s);
                    if (r < 0 && slept->errnum != INET_ERR_ESCAPE)
                        return slept;
                    inet_escape();                      /* acknowledged, as a pressed Escape is */
                    return inet_printf("\n");
                }
                if (r <= 0)
                    continue;
                char cbuf[512], buf[64];
                struct sockaddr_in off;
                struct iovec iov = { buf, sizeof buf };
                struct msghdr m = { .msg_name = &off, .msg_namelen = sizeof off, .msg_iov = &iov,
                                    .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
                if (recvmsg(s, &m, MSG_ERRQUEUE) < 0) {
                    recv(s, buf, sizeof buf, MSG_DONTWAIT);   /* a UDP reply: the host answered */
                    continue;
                }
                double T = now_ms() - t1;
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
                    if (cm->cmsg_level != IPPROTO_IP || cm->cmsg_type != IP_RECVERR)
                        continue;
                    struct sock_extended_err *ee = (struct sock_extended_err *)CMSG_DATA(cm);
                    if (ee->ee_origin != SO_EE_ORIGIN_ICMP)
                        continue;
                    struct sockaddr_in *from = (struct sockaddr_in *)SO_EE_OFFENDER(ee);
                    if (!got || from->sin_addr.s_addr != last) {
                        if (got)
                            e = inet_printf("\n   ");
                        char host[NI_MAXHOST];
                        if (!numeric && getnameinfo((struct sockaddr *)from, sizeof *from, host, sizeof host,
                                                    NULL, 0, NI_NAMEREQD) == 0)
                            e = inet_printf(" %s (%s)", host, inet_ntoa(from->sin_addr));
                        else
                            e = inet_printf(" %s", inet_ntoa(from->sin_addr));
                        last = from->sin_addr.s_addr;
                        got = 1;
                    }
                    e = inet_printf("  %.3f ms", T);
                    if (ee->ee_type == 3) {             /* unreachable */
                        switch (ee->ee_code) {
                        case 3:                         /* port: the host itself */
                            done = 1;
                            break;
                        case 0:
                            e = inet_printf(" !N"), done = 1;
                            break;
                        case 1:
                            e = inet_printf(" !H"), done = 1;
                            break;
                        case 2:
                            e = inet_printf(" !P"), done = 1;
                            break;
                        case 4:
                            e = inet_printf(" !F-%u", ee->ee_info), done = 1;
                            break;
                        case 13:
                            e = inet_printf(" !X"), done = 1;
                            break;
                        default:
                            e = inet_printf(" !<%u>", ee->ee_code), done = 1;
                            break;
                        }
                    }
                    answered = 1;
                }
            }
            if (!answered)
                e = inet_printf(" *");
        }
        if (!e)
            e = inet_printf("\n");
    }
    close(s);
    return e;
#endif
}
