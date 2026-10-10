/*
 * SPDX-License-Identifier: BSD-4-Clause
 *
 * Copyright (c) 2013 Gleb Smirnoff <glebius@FreeBSD.org>
 * Copyright (c) 1983, 1988, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's netstat(1)
 * as ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/inetstat: c.main, c.inet, c.if, c.route).
 * The 4-clause terms of c.inet are kept for the whole file.
 */

/* inetstat.c -- *InetStat (InetRes/Sources/inetstat, FreeBSD's netstat),
 * over Linux's /proc/net.
 *
 *   (none)  Internet sockets, connected ones only, or with -a the
 *           listeners too. It prints "Active Internet connections", then
 *           Proto, Recv-Q, Send-Q, Local and Foreign Address (host.port,
 *           "*" for any) and the TCP state by BSD's name, from
 *           /proc/net/tcp and udp;
 *   -r      "Routing tables", Internet: Destination, Gateway, Flags, Netif,
 *           from /proc/net/route;
 *   -i      Name, Mtu, Network, Address, Ipkts, Ierrs, Idrop, Opkts, Oerrs,
 *           Coll, from getifaddrs and /proc/net/dev;
 *   -s      the protocols' counters that Linux keeps (/proc/net/snmp), each
 *           in FreeBSD's words. These are fewer than FreeBSD counts.
 * -n is numeric. -p tcp|udp|ip|icmp selects one protocol. */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "inetres.h"

static const struct inet_tool tool = { "inetstat", 0 };

#ifdef __linux__
static const char *const tcpstates[] = { "", "ESTABLISHED", "SYN_SENT", "SYN_RCVD", "FIN_WAIT_1", "FIN_WAIT_2",
                                         "TIME_WAIT", "CLOSED", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING" };

static void inetname(uint32_t a, int numeric, char *out, size_t max)
{
    struct in_addr in = { a };
    if (a == INADDR_ANY) {
        snprintf(out, max, "*");
        return;
    }
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_addr = in };
    char host[NI_MAXHOST];
    if (!numeric && !getnameinfo((struct sockaddr *)&sin, sizeof sin, host, sizeof host, NULL, 0, NI_NAMEREQD)) {
        char *dot = strchr(host, '.');                  /* the local domain off, as inetname's */
        if (dot)
            *dot = 0;
        snprintf(out, max, "%s", host);
    } else {
        snprintf(out, max, "%s", inet_ntoa(in));
    }
}

static os_error *inetprint(uint32_t a, unsigned port, const char *proto, int numeric)
{
    char line[80], host[64];
    inetname(a, numeric, host, sizeof host);
    int n = snprintf(line, sizeof line, "%.16s.", host);
    struct servent *sp = !numeric && port ? getservbyport((int)htons((uint16_t)port), proto) : NULL;
    if (sp || !port)
        snprintf(line + n, sizeof line - (size_t)n, "%.15s ", sp ? sp->s_name : "*");
    else
        snprintf(line + n, sizeof line - (size_t)n, "%u ", port);
    return inet_printf("%-22.22s ", line);
}

static os_error *sockets(const char *proto, int all, int numeric, int *first)
{
    char path[32];
    snprintf(path, sizeof path, "/proc/net/%s", proto);
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char line[512];
    os_error *e = NULL;
    if (fgets(line, sizeof line, f))
        while (!e && fgets(line, sizeof line, f)) {
            unsigned la, lp, fa, fp, st;
            unsigned long txq, rxq;
            if (sscanf(line, " %*d: %x:%x %x:%x %x %lx:%lx", &la, &lp, &fa, &fp, &st, &txq, &rxq) != 7)
                continue;
            int tcp = !strcmp(proto, "tcp");
            if (!all && (fa == 0 || (tcp && st == 10)))
                continue;                               /* a server: -a's */
            if (*first) {
                e = inet_printf("Active Internet connections%s\n", all ? " (including servers)" : "");
                if (!e)
                    e = inet_printf("%-5.5s %-6.6s %-6.6s  %-22.22s %-22.22s %s\n", "Proto", "Recv-Q", "Send-Q",
                                    "Local Address", "Foreign Address", "(state)");
                *first = 0;
            }
            if (!e)
                e = inet_printf("%-3.3s%-2.2s %6lu %6lu  ", proto, "4 ", rxq, txq);
            if (!e)
                e = inetprint(la, lp, proto, numeric);
            if (!e)
                e = inetprint(fa, fp, proto, numeric);
            if (!e && tcp && st < sizeof tcpstates / sizeof tcpstates[0])
                e = inet_printf("%s", tcpstates[st]);
            if (!e)
                e = inet_printf("\n");
        }
    fclose(f);
    return e;
}

static os_error *routes(int numeric)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return NULL;
    os_error *e = inet_printf("Routing tables\n\nInternet:\n%-18.18s %-18.18s %-6.6s %8.8s %6s\n", "Destination",
                              "Gateway", "Flags", "Netif", "Expire");
    char line[256];
    if (fgets(line, sizeof line, f))
        while (!e && fgets(line, sizeof line, f)) {
            char dev[16];
            unsigned dst, gw, flags, mask;
            if (sscanf(line, "%15s %x %x %x %*u %*u %*d %x", dev, &dst, &gw, &flags, &mask) != 5)
                continue;
            char d[80], g[80], fl[8] = "", rif[16];
            if (!dst && !mask) {
                snprintf(d, sizeof d, "default");
            } else {
                char host[64];
                inetname(dst, numeric, host, sizeof host);
                int bits = __builtin_popcount(mask);
                if (bits == 32)
                    snprintf(d, sizeof d, "%s", host);
                else
                    snprintf(d, sizeof d, "%s/%d", inet_ntoa((struct in_addr){ dst }), bits);
            }
            if (flags & 2)
                inetname(gw, numeric, g, sizeof g);
            else
                snprintf(g, sizeof g, "link#%u", if_nametoindex(dev));
            size_t k = 0;
            if (flags & 1)
                fl[k++] = 'U';
            if (flags & 2)
                fl[k++] = 'G';
            if (flags & 4)
                fl[k++] = 'H';
            fl[k++] = 'S';
            fl[k] = 0;
            inet_riscos_ifname(dev, rif);
            e = inet_printf("%-18.18s %-18.18s %-6.6s %8.8s %6s\n", d, g, fl, rif, "");
        }
    fclose(f);
    return e;
}

static int dev_stats(const char *name, unsigned long long v[16])
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f)
        return -1;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *n = line;
        while (*n == ' ')
            n++;
        if (strcmp(n, name))
            continue;
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11],
                   &v[12], &v[13], &v[14], &v[15]) == 16)
            found = 0;
    }
    fclose(f);
    return found;
}

static os_error *interfaces(void)
{
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0)
        return NULL;
    os_error *e = inet_printf("%-5.5s %5.5s %-13.13s %-17.17s %8.8s %5.5s %5.5s %8.8s %5.5s %5s\n", "Name", "Mtu",
                              "Network", "Address", "Ipkts", "Ierrs", "Idrop", "Opkts", "Oerrs", "Coll");
    for (struct ifaddrs *x = all; !e && x; x = x->ifa_next) {
        if (!x->ifa_addr || x->ifa_addr->sa_family != AF_INET)
            continue;
        unsigned long long v[16] = { 0 };
        dev_stats(x->ifa_name, v);
        char rif[16], net[32], mtu[16] = "";
        inet_riscos_ifname(x->ifa_name, rif);
        uint32_t a = ((struct sockaddr_in *)x->ifa_addr)->sin_addr.s_addr;
        uint32_t m = x->ifa_netmask ? ((struct sockaddr_in *)x->ifa_netmask)->sin_addr.s_addr : 0xFFFFFFFFu;
        snprintf(net, sizeof net, "%s/%d", inet_ntoa((struct in_addr){ a & m }), __builtin_popcount(m));
        FILE *f;
        char path[64];
        snprintf(path, sizeof path, "/sys/class/net/%s/mtu", x->ifa_name);
        if ((f = fopen(path, "r"))) {
            if (!fgets(mtu, sizeof mtu, f))
                mtu[0] = 0;
            mtu[strcspn(mtu, "\n")] = 0;
            fclose(f);
        }
        e = inet_printf("%-5.5s %5.5s %-13.13s %-17.17s %8llu %5llu %5llu %8llu %5llu %5llu\n", rif, mtu, net,
                        inet_ntoa((struct in_addr){ a }), v[1], v[2], v[3], v[9], v[10], v[13]);
    }
    freeifaddrs(all);
    return e;
}

/* One protocol's line pair from /proc/net/snmp: names, then values. */
static int snmp(const char *proto, const char *key, unsigned long long *out)
{
    FILE *f = fopen("/proc/net/snmp", "r");
    if (!f)
        return -1;
    char names[2048], values[2048];
    int found = -1;
    size_t pl = strlen(proto);
    while (fgets(names, sizeof names, f) && fgets(values, sizeof values, f)) {
        if (strncmp(names, proto, pl) || names[pl] != ':')
            continue;
        char *ns, *vs, *n = strtok_r(names + pl + 1, " \n", &ns), *v = strtok_r(values + pl + 1, " \n", &vs);
        for (; n && v; n = strtok_r(NULL, " \n", &ns), v = strtok_r(NULL, " \n", &vs))
            if (!strcmp(n, key)) {
                *out = strtoull(v, NULL, 10);
                found = 0;
            }
    }
    fclose(f);
    return found;
}

static os_error *stats(const char *only)
{
    static const struct { const char *proto, *label, *key, *text; } lines[] = {
        { "Tcp", "tcp", "OutSegs", "packets sent" },
        { "Tcp", "tcp", "RetransSegs", "data packets retransmitted" },
        { "Tcp", "tcp", "OutRsts", "control packets (resets)" },
        { "Tcp", "tcp", "InSegs", "packets received" },
        { "Tcp", "tcp", "InErrs", "discarded for bad checksums" },
        { "Tcp", "tcp", "ActiveOpens", "connection requests" },
        { "Tcp", "tcp", "PassiveOpens", "connection accepts" },
        { "Tcp", "tcp", "AttemptFails", "bad connection attempts" },
        { "Tcp", "tcp", "EstabResets", "connections dropped" },
        { "Tcp", "tcp", "CurrEstab", "connections established" },
        { "Udp", "udp", "InDatagrams", "datagrams received" },
        { "Udp", "udp", "InErrors", "with bad data length field" },
        { "Udp", "udp", "NoPorts", "dropped due to no socket" },
        { "Udp", "udp", "RcvbufErrors", "dropped due to full socket buffers" },
        { "Udp", "udp", "OutDatagrams", "datagrams output" },
        { "Ip", "ip", "InReceives", "total packets received" },
        { "Ip", "ip", "InHdrErrors", "bad header checksums" },
        { "Ip", "ip", "ReasmReqds", "fragments received" },
        { "Ip", "ip", "ReasmFails", "fragments dropped (dup or out of space)" },
        { "Ip", "ip", "ForwDatagrams", "packets forwarded" },
        { "Ip", "ip", "InUnknownProtos", "packets for unknown/unsupported protocol" },
        { "Ip", "ip", "InDelivers", "packets for this host" },
        { "Ip", "ip", "OutRequests", "packets sent from this host" },
        { "Ip", "ip", "OutNoRoutes", "output packets discarded due to no route" },
        { "Ip", "ip", "FragCreates", "output datagrams fragmented" },
        { "Icmp", "icmp", "OutMsgs", "calls to icmp_error" },
        { "Icmp", "icmp", "InErrors", "messages with bad code fields" },
        { "Icmp", "icmp", "InEchos", "echo requests received" },
        { "Icmp", "icmp", "OutEchoReps", "echo replies sent" },
        { "Icmp", "icmp", "InDestUnreachs", "destination unreachables received" },
    };
    os_error *e = NULL;
    const char *heading = NULL;
    for (unsigned i = 0; !e && i < sizeof lines / sizeof lines[0]; i++) {
        if (only && strcmp(only, lines[i].label))
            continue;
        unsigned long long v;
        if (snmp(lines[i].proto, lines[i].key, &v))
            continue;
        if (heading != lines[i].label) {
            e = inet_printf("%s:\n", lines[i].label);
            heading = lines[i].label;
        }
        if (!e)
            e = inet_printf("\t%llu %s\n", v, lines[i].text);
    }
    return e;
}
#endif

os_error *inet_inetstat(const struct inet_args *a)
{
#ifndef __linux__
    (void)a;
    return inet_fail(&tool, 1, "not available on this host: ROSGD's InetStat reads Linux's /proc/net");
#else
    struct inet_opt o = { 0 };
    int all = 0, numeric = 0, r = 0, i = 0, s = 0, c;
    const char *proto = NULL;
    while ((c = inet_getopt(&o, a, "anrisp:f:ALSWbdM:N:w:I:q:z")) != -1) {
        switch (c) {
        case 'a':
            all = 1;
            break;
        case 'n':
            numeric = 1;
            break;
        case 'r':
            r = 1;
            break;
        case 'i':
            i = 1;
            break;
        case 's':
            s = 1;
            break;
        case 'p':
            proto = o.arg;
            break;
        case 'f':
            break;                                      /* inet is all there is */
        case '?':
            return inet_fail(&tool, 1, "illegal option -- %c", a->argv[o.ind - 1][1]);
        default:
            return inet_fail(&tool, 1, "option -%c is not carried by ROSGD's inetstat", c);
        }
    }
    if (s)
        return stats(proto);
    if (r)
        return routes(numeric);
    if (i)
        return interfaces();
    int first = 1;
    os_error *e = NULL;
    if (!proto || !strcmp(proto, "tcp"))
        e = sockets("tcp", all, numeric, &first);
    if (!e && (!proto || !strcmp(proto, "udp")))
        e = sockets("udp", all, numeric, &first);
    return e;
#endif
}
