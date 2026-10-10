/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1983, 1989, 1991, 1993
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
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's route(8)
 * as ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/route: c.Route).
 */

/* route.c -- *Route (InetRes/Sources/route, FreeBSD's route), over Linux:
 * /proc/net/route to read the table, SIOCADDRT and SIOCDELRT to change it.
 *
 *   add|delete|change [-net|-host] <dest> [<gateway>] [netmask <mask>]
 *       prints "add net default: gateway 10.0.2.2", or ": <reason>" on a
 *       failure. It prints nothing with -e, where a failure goes to
 *       Inet$Error;
 *   get|show <dest>
 *       FreeBSD's print_getmsg: route to, destination, mask, gateway,
 *       interface, flags, and its row of metrics;
 *   flush
 *       every route through a gateway, "dest gateway done" each.
 * A destination is a host with -host or four parts. It is a net with -net
 * or fewer parts, with its mask by class as BSD's inet_makenetandmask
 * gives it, "default" (0.0.0.0/0), or <net>/<bits>. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <net/route.h>
#endif

#include "inetres.h"

#ifdef __linux__
struct entry {
    char dev[16];
    uint32_t dst, gw, mask;                 /* network order */
    unsigned flags;
    int metric, mtu;
};

static int read_table(struct entry *out, int max)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return 0;
    char line[256];
    int n = 0;
    if (fgets(line, sizeof line, f))
        while (n < max && fgets(line, sizeof line, f)) {
            struct entry *e = &out[n];
            unsigned refcnt, use, window, irtt;
            if (sscanf(line, "%15s %x %x %x %u %u %d %x %d %u %u", e->dev, &e->dst, &e->gw, &e->flags, &refcnt,
                       &use, &e->metric, &e->mask, &e->mtu, &window, &irtt) == 11)
                n++;
        }
    fclose(f);
    return n;
}

static void routename(uint32_t a, int numeric, char *out, size_t max)
{
    struct in_addr in = { a };
    if (!a) {
        snprintf(out, max, "default");
        return;
    }
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_addr = in };
    if (numeric || getnameinfo((struct sockaddr *)&sin, sizeof sin, out, max, NULL, 0, NI_NAMEREQD))
        snprintf(out, max, "%s", inet_ntoa(in));
}

/* BSD's inet_makenetandmask: a number of fewer than four parts is a net,
 * its mask by class. */
static int parse_dest(const char *s, int force_net, int force_host, uint32_t *dst, uint32_t *mask, int *is_host)
{
    if (!strcmp(s, "default")) {
        *dst = 0, *mask = 0, *is_host = 0;
        return 0;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    char *slash = strchr(buf, '/');
    long bits = -1;
    if (slash) {
        *slash = 0;
        bits = strtol(slash + 1, NULL, 10);
        if (bits < 0 || bits > 32)
            return -1;
    }
    unsigned p[4] = { 0 };
    int parts = sscanf(buf, "%u.%u.%u.%u", &p[0], &p[1], &p[2], &p[3]);
    if (parts < 1) {
        struct addrinfo hints = { .ai_family = AF_INET }, *res;
        if (getaddrinfo(buf, NULL, &hints, &res))
            return -1;
        *dst = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        *mask = 0xFFFFFFFFu;
        *is_host = !force_net;
        return 0;
    }
    uint32_t v = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    *is_host = force_host || (!force_net && parts == 4 && bits < 0);
    if (*is_host) {
        *dst = htonl(v);
        *mask = 0xFFFFFFFFu;
        return 0;
    }
    uint32_t m = bits >= 0 ? (bits ? 0xFFFFFFFFu << (32 - bits) : 0)
                 : p[0] < 128 ? 0xFF000000u : p[0] < 192 ? 0xFFFF0000u : 0xFFFFFF00u;
    *dst = htonl(v & m);
    *mask = htonl(m);
    return 0;
}

static const char *errmsg(int e)
{
    switch (e) {
    case ESRCH:
        return "not in table";
    case EBUSY:
        return "entry in use";
    case ENOBUFS:
        return "not enough memory";
    case EADDRINUSE:
        return "gateway uses the same route";
    case EEXIST:
        return "route already in table";
    default:
        return strerror(e);
    }
}

static os_error *show_flags(unsigned f)
{
    static const struct { unsigned bit; const char *name; } names[] = {
        { RTF_UP, "UP" }, { RTF_GATEWAY, "GATEWAY" }, { RTF_HOST, "HOST" }, { RTF_REJECT, "REJECT" },
        { RTF_DYNAMIC, "DYNAMIC" }, { RTF_MODIFIED, "MODIFIED" },
    };
    os_error *e = inet_printf("<");
    int first = 1;
    for (unsigned i = 0; !e && i < sizeof names / sizeof names[0]; i++)
        if (f & names[i].bit) {
            e = inet_printf("%s%s", first ? "" : ",", names[i].name);
            first = 0;
        }
    if (!e)                                  /* every Linux route is complete and set by someone */
        e = inet_printf("%sDONE,STATIC>", first ? "" : ",");
    return e;
}
#endif

os_error *inet_route(const struct inet_args *a)
{
    struct inet_tool t = { "Route", 0 };
#ifndef __linux__
    (void)a;
    return inet_fail(&t, 1, "not available on this host: ROSGD's Route changes Linux's table");
#else
    struct inet_opt o = { 0 };
    int numeric = 0, quiet = 0, c;
    while ((c = inet_getopt(&o, a, "46denqtv")) != -1) {
        switch (c) {
        case 'e':
            t.e_flag = 1;
            break;
        case 'n':
            numeric = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        case '4':
        case 'd':
        case 't':
        case 'v':
            break;
        default:
            return inet_printf("Usage:   route [options] add|delete|del|change|get|show [-net|-host] <dest> "
                               "[<gateway>] [netmask] [flags]\n"
                               "         route [options] flush\n");
        }
    }
    if (o.ind >= a->argc)
        return inet_printf("Usage:   route [options] add|delete|del|change|get|show [-net|-host] <dest> "
                           "[<gateway>] [netmask] [flags]\n");
    const char *cmd = a->argv[o.ind++];
    struct entry tab[256];
    int n = read_table(tab, 256);
    os_error *e = NULL;

    if (!strcmp(cmd, "flush")) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        for (int i = 0; !e && i < n; i++) {
            if (!(tab[i].flags & RTF_GATEWAY))
                continue;
            struct rtentry r;
            memset(&r, 0, sizeof r);
            ((struct sockaddr_in *)&r.rt_dst)->sin_family = AF_INET;
            ((struct sockaddr_in *)&r.rt_dst)->sin_addr.s_addr = tab[i].dst;
            ((struct sockaddr_in *)&r.rt_genmask)->sin_family = AF_INET;
            ((struct sockaddr_in *)&r.rt_genmask)->sin_addr.s_addr = tab[i].mask;
            ((struct sockaddr_in *)&r.rt_gateway)->sin_family = AF_INET;
            ((struct sockaddr_in *)&r.rt_gateway)->sin_addr.s_addr = tab[i].gw;
            r.rt_dev = tab[i].dev;
            if (ioctl(s, SIOCDELRT, &r) < 0)
                continue;
            if (!quiet && !t.e_flag) {
                char d[256], g[256];
                routename(tab[i].dst, numeric, d, sizeof d);
                routename(tab[i].gw, numeric, g, sizeof g);
                e = inet_printf("%-20.20s %-20.20s done\n", d, g);
            }
        }
        if (s >= 0)
            close(s);
        return e;
    }

    int is_add = !strcmp(cmd, "add"), is_del = !strcmp(cmd, "delete") || !strcmp(cmd, "del"),
        is_change = !strcmp(cmd, "change"), is_get = !strcmp(cmd, "get") || !strcmp(cmd, "show");
    if (!is_add && !is_del && !is_change && !is_get)
        return inet_fail(&t, 64, "%s: invalid command", cmd);
    int force_net = 0, force_host = 0;
    const char *dest = NULL, *gateway = NULL, *netmask = NULL;
    for (int i = o.ind; i < a->argc; i++) {
        const char *w = a->argv[i];
        if (!strcmp(w, "-net"))
            force_net = 1;
        else if (!strcmp(w, "-host"))
            force_host = 1;
        else if ((!strcmp(w, "-netmask") || !strcmp(w, "netmask")) && i + 1 < a->argc)
            netmask = a->argv[++i];
        else if (w[0] == '-')
            continue;                       /* -interface, -static and the like: Linux's own */
        else if (!dest)
            dest = w;
        else if (!gateway)
            gateway = w;
        else if (!netmask)
            netmask = w;
    }
    if (!dest)
        return inet_fail(&t, 64, "destination required");
    uint32_t dst, mask;
    int is_host;
    if (parse_dest(dest, force_net, force_host, &dst, &mask, &is_host))
        return inet_fail(&t, 1, "bad address: %s", dest);
    if (netmask) {
        struct in_addr m;
        if (!inet_aton(netmask, &m))
            return inet_fail(&t, 1, "bad address: %s", netmask);
        mask = m.s_addr;
        is_host = 0;
        dst &= mask;
    }

    if (is_get) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if ((dst & tab[i].mask) == tab[i].dst && (best < 0 || ntohl(tab[i].mask) > ntohl(tab[best].mask)))
                best = i;
        char to[256];
        routename(dst, numeric, to, sizeof to);
        if (best < 0) {
            inet_exit(1);
            return inet_printf("   route to: %s\nRoute: route has not been found\n", to);
        }
        char d[256], m[256], g[256], rif[16];
        routename(tab[best].dst, numeric, d, sizeof d);
        struct in_addr mk = { tab[best].mask };
        snprintf(m, sizeof m, "%s", tab[best].mask ? inet_ntoa(mk) : "default");
        routename(tab[best].gw, numeric, g, sizeof g);
        inet_riscos_ifname(tab[best].dev, rif);
        e = inet_printf("   route to: %s\ndestination: %s\n       mask: %s\n", to, d, m);
        if (!e && (tab[best].flags & RTF_GATEWAY))
            e = inet_printf("    gateway: %s\n", g);
        if (!e)
            e = inet_printf("  interface: %s\n      flags: ", rif);
        if (!e)
            e = show_flags(tab[best].flags);
        if (!e)
            e = inet_printf("\n%9s %9s %9s %9s %9s %10s %9s\n", "recvpipe", "sendpipe", "ssthresh", "rtt,msec",
                            "mtu   ", "weight", "expire");
        if (!e)
            e = inet_printf("%8u  %8u  %8u  %8u  %8u  %8u  %8d \n", 0u, 0u, 0u, 0u, (unsigned)tab[best].mtu,
                            1u, 0);
        return e;
    }

    struct rtentry r;
    memset(&r, 0, sizeof r);
    struct sockaddr_in *sd = (struct sockaddr_in *)&r.rt_dst, *sm = (struct sockaddr_in *)&r.rt_genmask,
                       *sg = (struct sockaddr_in *)&r.rt_gateway;
    sd->sin_family = sm->sin_family = sg->sin_family = AF_INET;
    sd->sin_addr.s_addr = dst;
    sm->sin_addr.s_addr = mask;
    r.rt_flags = RTF_UP | (is_host ? RTF_HOST : 0);
    char gw_text[64] = "";
    char dev[16];
    if (gateway) {
        struct in_addr g;
        char lname[16];
        if (inet_aton(gateway, &g)) {
            sg->sin_addr = g;
            r.rt_flags |= RTF_GATEWAY;
            snprintf(gw_text, sizeof gw_text, "%s", gateway);
        } else if (!inet_linux_ifname(gateway, lname)) {        /* an interface: a direct route */
            snprintf(dev, sizeof dev, "%s", lname);
            r.rt_dev = dev;
            snprintf(gw_text, sizeof gw_text, "%s", gateway);
        } else {
            return inet_fail(&t, 1, "bad address: %s", gateway);
        }
    } else if (!is_del) {
        return inet_fail(&t, 64, "gateway required");
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0), rc = 0, err = 0;
    if (is_change) {
        ioctl(s, SIOCDELRT, &r);
        is_add = 1;
    }
    rc = ioctl(s, is_add ? SIOCADDRT : SIOCDELRT, &r);
    err = errno;
    close(s);
    if (rc < 0 && err == ENOENT)
        err = ESRCH;
    char text[256];
    snprintf(text, sizeof text, "%s %s %s", cmd, is_host ? "host" : "net", dest);
    if (*gw_text)
        snprintf(text + strlen(text), sizeof text - strlen(text), ": gateway %s", gw_text);
    if (rc < 0) {
        if (t.e_flag)
            return inet_fail(&t, 1, "%s: %s", text, errmsg(err));
        inet_exit(1);
        return quiet ? NULL : inet_printf("%s: %s\n", text, errmsg(err));
    }
    if (quiet || t.e_flag)
        return NULL;
    return inet_printf("%s\n", text);
#endif
}
