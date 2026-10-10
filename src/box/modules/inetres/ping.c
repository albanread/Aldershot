/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Muuss.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's ping(8) as
 * ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/ping: c.Ping, c.utils).
 */

/* ping.c -- *Ping (InetRes/Sources/ping, FreeBSD's ping), over Linux's ICMP
 * sockets.
 *
 * Its waits are sleeps (ros_sleep_fd), so in a task window the desktop goes
 * on while it waits for a reply or the next second.
 *
 * It sends an echo request a second (-i) until Escape, -c's count or -t's
 * timeout. Each reply is printed as FreeBSD's pr_pack prints it, and then
 * the statistics that finish() prints. Sys$ReturnCode is 0 if anything
 * came back, else 2. The socket is Linux's unprivileged ICMP datagram
 * socket (the kernel fills in the identifier and checksum), or a raw one
 * where that is refused. */
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "inetres.h"

/* musl's CMSG_NXTHDR compares a size_t with a long */
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "rosgd/background.h"

#define DEFDATALEN 56
#define ICMP_HDR   8
#define MAXPACKET  (65535 - 60 - ICMP_HDR)

static double now_ms(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

static uint16_t cksum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (; n > 1; p += 2, n -= 2)
        s += (uint32_t)(p[0] | p[1] << 8);
    if (n)
        s += p[0];
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* "name (address)", or the address alone with -n or no name */
static void pr_addr(struct in_addr a, int numeric, char *out, size_t max)
{
    char host[NI_MAXHOST];
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_addr = a };
    if (!numeric && getnameinfo((struct sockaddr *)&sin, sizeof sin, host, sizeof host, NULL, 0,
                                NI_NAMEREQD) == 0)
        snprintf(out, max, "%s (%s)", host, inet_ntoa(a));
    else
        snprintf(out, max, "%s", inet_ntoa(a));
}

os_error *inet_ping(const struct inet_args *a)
{
    struct inet_tool t = { "ping", 0 };
    struct inet_opt o = { 0 };
    long count = 0, datalen = DEFDATALEN, ttl = 0, timeout = 0;
    double interval = 1000, waittime = 10000;
    int numeric = 0, quiet = 0, c;
    while ((c = inet_getopt(&o, a, "c:i:m:ns:t:W:qDdfLoQRrvG:g:h:l:M:p:S:z:")) != -1) {
        switch (c) {
        case 'c':
            count = strtol(o.arg, NULL, 10);
            if (count <= 0)
                return inet_fail(&t, 64, "invalid count of packets to transmit: `%s'", o.arg);
            break;
        case 'i':
            interval = strtod(o.arg, NULL) * 1000;
            if (interval < 1000 && getuid() != 0)
                return inet_fail(&t, 1, "-i interval too short: Operation not permitted");
            break;
        case 'm':
            ttl = strtol(o.arg, NULL, 10);
            if (ttl <= 0 || ttl > 255)
                return inet_fail(&t, 64, "invalid TTL: `%s'", o.arg);
            break;
        case 'n':
            numeric = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        case 's':
            datalen = strtol(o.arg, NULL, 10);
            if (datalen < 0 || datalen > MAXPACKET)
                return inet_fail(&t, 64, "invalid packet size: `%s'", o.arg);
            break;
        case 't':
            timeout = strtol(o.arg, NULL, 10);
            if (timeout <= 0)
                return inet_fail(&t, 64, "invalid timeout: `%s'", o.arg);
            break;
        case 'W':
            waittime = strtod(o.arg, NULL);
            break;
        case '?':
            return inet_fail(&t, 64, "illegal option -- %c", a->argv[o.ind - 1][1]);
        case ':':
            return inet_fail(&t, 64, "option requires an argument -- %c", a->argv[o.ind - 1][1]);
        default:
            return inet_fail(&t, 64, "option -%c is not carried by ROSGD's ping", c);
        }
    }
    if (o.ind != a->argc - 1)
        return inet_printf("Usage:   ping [-nq] [-c count] [-i wait] [-m ttl] [-s packetsize] [-t timeout]\n"
                           "              [-W waittime] <host>\n");
    const char *target = a->argv[o.ind];

    struct addrinfo hints = { .ai_family = AF_INET }, *res;
    int gai = getaddrinfo(target, NULL, &hints, &res);
    if (gai)
        return inet_fail(&t, 68, "cannot resolve %s: %s", target,
                         gai == EAI_NONAME ? "Unknown host" : gai_strerror(gai));
    struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    char hostname[256];
    struct in_addr literal;
    if (inet_aton(target, &literal))
        snprintf(hostname, sizeof hostname, "%s", inet_ntoa(to.sin_addr));
    else
        snprintf(hostname, sizeof hostname, "%s", target);

    int raw = 0, s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s < 0) {
        s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        raw = 1;
    }
    if (s < 0)
        return inet_fail(&t, 71, "socket: %s", strerror(errno));
    if (ttl)
        setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
    int one = 1;
#ifdef IP_RECVTTL
    setsockopt(s, IPPROTO_IP, IP_RECVTTL, &one, sizeof one);
#endif
    (void)one;

    os_error *e = inet_printf("PING %s (%s): %ld data bytes\n", hostname, inet_ntoa(to.sin_addr), datalen);
    uint16_t ident = (uint16_t)getpid(), seq = 0;
    long transmitted = 0, received = 0, repeats = 0;
    double tmin = 1e9, tmax = 0, tsum = 0, tsumsq = 0;
    uint8_t *out = calloc(1, (size_t)datalen + ICMP_HDR), in[65536];
    uint8_t seen[8192] = { 0 };
    double start = now_ms(), next = start;
    int stop = 0;

    while (!e && !stop) {
        double t0 = now_ms();
        if (t0 >= next && (!count || transmitted < count)) {
            memset(out, 0, (size_t)datalen + ICMP_HDR);
            out[0] = 8;                             /* ICMP_ECHO */
            out[4] = (uint8_t)(ident >> 8), out[5] = (uint8_t)ident;
            out[6] = (uint8_t)(seq >> 8), out[7] = (uint8_t)seq;
            if (datalen >= (long)sizeof(double))
                memcpy(out + ICMP_HDR, &t0, sizeof t0);
            for (long i = sizeof(double); i < datalen; i++)
                out[ICMP_HDR + i] = (uint8_t)i;
            uint16_t sum = cksum(out, (size_t)datalen + ICMP_HDR);
            out[2] = (uint8_t)sum, out[3] = (uint8_t)(sum >> 8);
            if (sendto(s, out, (size_t)datalen + ICMP_HDR, 0, (struct sockaddr *)&to, sizeof to) < 0)
                e = inet_printf("ping: sendto: %s\n", strerror(errno));
            transmitted++;
            seq++;
            next = t0 + interval;
        }
        if (inet_escape())
            break;
        if (timeout && now_ms() - start >= timeout * 1000.0)
            break;
        if (count && transmitted >= count && (received >= count || now_ms() - next + interval >= waittime))
            break;

        int wait = (int)(next - now_ms());
        if (count && transmitted >= count)
            wait = 100;
        if (wait > 100)
            wait = 100;
        if (wait < 0)
            wait = 0;
        os_error *slept;
        int ready = ros_sleep_fd(s, POLLIN, (unsigned)wait, &slept);
        if (ready < 0) {
            if (slept->errnum != INET_ERR_ESCAPE)
                e = slept;
            inet_escape();                          /* acknowledged, as a pressed Escape is */
            break;                                  /* Escape: the statistics */
        }
        if (!ready)
            continue;

        struct sockaddr_in from;
        char cbuf[64];
        struct iovec iov = { in, sizeof in };
        struct msghdr m = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov,
                            .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
        ssize_t n = recvmsg(s, &m, 0);
        if (n <= 0)
            continue;
        uint8_t *icmp = in;
        int rttl = -1;
        if ((raw || (in[0] >> 4) == 4) && n >= 20 && (in[0] >> 4) == 4) {
            size_t hl = (size_t)(in[0] & 15) * 4;   /* an IP header in front (raw, or macOS) */
            rttl = in[8];
            icmp = in + hl;
            n -= (ssize_t)hl;
        }
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
            if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TTL)
                rttl = *(int *)CMSG_DATA(cm);
        if (n < ICMP_HDR || icmp[0] != 0)           /* ICMP_ECHOREPLY */
            continue;
        uint16_t rseq = (uint16_t)(icmp[6] << 8 | icmp[7]);
        if (raw && (uint16_t)(icmp[4] << 8 | icmp[5]) != ident)
            continue;
        double t1 = now_ms(), sent = 0, trip = 0;
        int timing = n >= ICMP_HDR + (ssize_t)sizeof(double);
        if (timing) {
            memcpy(&sent, icmp + ICMP_HDR, sizeof sent);
            trip = t1 - sent;
        }
        int dup = seen[rseq & 8191];
        seen[rseq & 8191] = 1;
        if (dup) {
            repeats++;
        } else {
            received++;
            if (timing) {
                tsum += trip, tsumsq += trip * trip;
                if (trip < tmin)
                    tmin = trip;
                if (trip > tmax)
                    tmax = trip;
            }
        }
        if (!quiet) {
            char who[400];
            pr_addr(from.sin_addr, numeric, who, sizeof who);
            e = inet_printf("%zd bytes from %s: icmp_seq=%u", n, who, rseq);
            if (!e && rttl >= 0)
                e = inet_printf(" ttl=%d", rttl);
            if (!e && timing)
                e = inet_printf(" time=%.3f ms", trip);
            if (!e && dup)
                e = inet_printf(" (DUP!)");
            if (!e)
                e = inet_printf("\n");
        }
    }
    close(s);
    free(out);
    if (e)
        return e;

    inet_printf("\n--- %s ping statistics ---\n", hostname);
    inet_printf("%ld packets transmitted, %ld packets received, ", transmitted, received);
    if (repeats)
        inet_printf("+%ld duplicates, ", repeats);
    if (transmitted) {
        if (received > transmitted)
            inet_printf("-- somebody's printing up packets!");
        else
            inet_printf("%.1f%% packet loss", (transmitted - received) * 100.0 / transmitted);
    }
    inet_printf("\n");
    if (received) {
        double nr = (double)(received + repeats), avg = tsum / nr;
        inet_printf("round-trip min/avg/max/stddev = %.3f/%.3f/%.3f/%.3f ms\n", tmin, avg, tmax,
                    sqrt(tsumsq / nr - avg * avg));
    }
    inet_exit(received ? 0 : 2);
    return NULL;
}
