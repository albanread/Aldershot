/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1983, 1993
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
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's
 * ifconfig(8) as ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/ifconfig: c.IfConfig, c.af_inet,
 * c.af_link).
 */

/* ifconfig.c -- *IfConfig (InetRes/Sources/ifconfig, FreeBSD's ifconfig),
 * over Linux: getifaddrs() to list, the SIOCSIF* ioctls to set.
 *
 * An interface prints as FreeBSD's status() prints it:
 *   "eth0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> metric 0 mtu 1500"
 *   "\tether 52:54:00:12:34:56"
 *   "\tinet 10.0.2.15 netmask 0xffffff00 broadcast 10.0.2.255"
 * with Linux's flags in BSD's bits (MULTICAST is &1000 in Linux, &8000 in
 * BSD) and SIMPLEX on Ethernet, as the Internet module's Ethernet layer
 * sets it. Names are RISC OS's: lo0 for lo, and a driver's name (ej0,
 * ege0) is Linux's Ethernet of that unit (inetres.h). Setting an address
 * marks the interface up, as BSD's does. -e sends errors to Inet$Error. */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/if_packet.h>
#else
#include <net/if_dl.h>
#endif

#include "inetres.h"

static const char *const bsd_bits[] = { "UP", "BROADCAST", "DEBUG", "LOOPBACK", "POINTOPOINT", NULL,
                                        "RUNNING", "NOARP", "PROMISC", "ALLMULTI", "OACTIVE", "SIMPLEX",
                                        "LINK0", "LINK1", "LINK2", "MULTICAST" };

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

static os_error *print_flags(unsigned b)
{
    os_error *e = inet_printf("flags=%x<", b);
    int first = 1;
    for (unsigned i = 0; !e && i < 16; i++)
        if (b & (1u << i) && bsd_bits[i]) {
            e = inet_printf("%s%s", first ? "" : ",", bsd_bits[i]);
            first = 0;
        }
    return e ? e : inet_printf(">");
}

static int ifr_ioctl(unsigned long req, const char *name, struct ifreq *r)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    snprintf(r->ifr_name, sizeof r->ifr_name, "%s", name);
    int rc = ioctl(s, req, r);
    int err = errno;
    close(s);
    errno = err;
    return rc;
}

/* One interface's status, from the list getifaddrs gave. */
static os_error *status(struct ifaddrs *all, const char *name)
{
    struct ifaddrs *first = NULL;
    for (struct ifaddrs *x = all; x; x = x->ifa_next)
        if (!strcmp(x->ifa_name, name)) {
            first = x;
            break;
        }
    if (!first)
        return NULL;
    char rname[16];
    inet_riscos_ifname(name, rname);
    struct ifreq r;
    memset(&r, 0, sizeof r);
    os_error *e = inet_printf("%s: ", rname);
    if (!e)
        e = print_flags(bsd_flags(first->ifa_flags));
    if (!e && ifr_ioctl(SIOCGIFMETRIC, name, &r) == 0)
        e = inet_printf(" metric %d", r.ifr_metric);
    if (!e && ifr_ioctl(SIOCGIFMTU, name, &r) == 0)
        e = inet_printf(" mtu %d", r.ifr_mtu);
    if (!e)
        e = inet_printf("\n");
    for (struct ifaddrs *x = all; !e && x; x = x->ifa_next) {
        if (strcmp(x->ifa_name, name) || !x->ifa_addr)
            continue;
        const unsigned char *mac = NULL;
#ifdef __linux__
        if (x->ifa_addr->sa_family == AF_PACKET) {
            struct sockaddr_ll *ll = (struct sockaddr_ll *)x->ifa_addr;
            if (ll->sll_halen == 6 && !(x->ifa_flags & IFF_LOOPBACK))
                mac = ll->sll_addr;
        }
#else
        if (x->ifa_addr->sa_family == AF_LINK) {
            struct sockaddr_dl *dl = (struct sockaddr_dl *)x->ifa_addr;
            if (dl->sdl_alen == 6)
                mac = (const unsigned char *)LLADDR(dl);
        }
#endif
        if (mac)
            e = inet_printf("\tether %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4],
                            mac[5]);
    }
    for (struct ifaddrs *x = all; !e && x; x = x->ifa_next) {
        if (strcmp(x->ifa_name, name) || !x->ifa_addr || x->ifa_addr->sa_family != AF_INET)
            continue;
        struct in_addr addr = ((struct sockaddr_in *)x->ifa_addr)->sin_addr;
        uint32_t mask = x->ifa_netmask ? ntohl(((struct sockaddr_in *)x->ifa_netmask)->sin_addr.s_addr) : 0;
        e = inet_printf("\tinet %s netmask 0x%lx", inet_ntoa(addr), (unsigned long)mask);
        if (!e && (x->ifa_flags & IFF_BROADCAST) && x->ifa_broadaddr)
            e = inet_printf(" broadcast %s", inet_ntoa(((struct sockaddr_in *)x->ifa_broadaddr)->sin_addr));
        if (!e)
            e = inet_printf("\n");
    }
    return e;
}

/* The interfaces' names, in the order getifaddrs gives them, once each. */
static int names(struct ifaddrs *all, char out[][16], int max)
{
    int n = 0;
    for (struct ifaddrs *x = all; x && n < max; x = x->ifa_next) {
        int seen = 0;
        for (int i = 0; i < n; i++)
            seen |= !strcmp(out[i], x->ifa_name);
        if (!seen)
            snprintf(out[n++], 16, "%s", x->ifa_name);
    }
    return n;
}

#ifdef __linux__
static int parse_in(const char *s, struct in_addr *a)
{
    if (!strncmp(s, "0x", 2)) {
        a->s_addr = htonl((uint32_t)strtoul(s, NULL, 16));
        return 0;
    }
    return inet_aton(s, a) ? 0 : -1;
}

static os_error *set_addr(const struct inet_tool *t, unsigned long req, const char *name, struct in_addr a,
                          const char *what)
{
    struct ifreq r;
    memset(&r, 0, sizeof r);
    struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr = a;
    if (ifr_ioctl(req, name, &r) < 0)
        return inet_fail(t, 1, "ioctl (%s): %s", what, strerror(errno));
    return NULL;
}
#endif

os_error *inet_ifconfig(const struct inet_args *a)
{
    struct inet_tool t = { "IfConfig", 0 };
    struct inet_opt o = { 0 };
    int all = 0, down = 0, up = 0, list = 0, c;
    while ((c = inet_getopt(&o, a, "adeluvCmn")) != -1) {
        switch (c) {
        case 'a':
            all = 1;
            break;
        case 'd':
            down = 1;
            break;
        case 'u':
            up = 1;
            break;
        case 'l':
            list = 1;
            break;
        case 'e':
            t.e_flag = 1;
            break;
        case 'v':
            break;
        case '?':
            return inet_fail(&t, 1, "illegal option -- %c", a->argv[o.ind - 1][1]);
        default:
            return inet_fail(&t, 1, "option -%c is not carried by ROSGD's IfConfig", c);
        }
    }
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0)
        return inet_fail(&t, 1, "getifaddrs: %s", strerror(errno));
    char nm[32][16];
    int n = names(ifs, nm, 32);
    os_error *e = NULL;

    if (o.ind >= a->argc || all || list || up || down) {    /* list them */
        int first = 1;
        for (int i = 0; !e && i < n; i++) {
            unsigned flags = 0;
            for (struct ifaddrs *x = ifs; x; x = x->ifa_next)
                if (!strcmp(x->ifa_name, nm[i]))
                    flags = x->ifa_flags;
            if ((down && (flags & IFF_UP)) || (up && !(flags & IFF_UP)))
                continue;
            if (list) {
                char r[16];
                inet_riscos_ifname(nm[i], r);
                e = inet_printf("%s%s", first ? "" : " ", r);
                first = 0;
            } else {
                e = status(ifs, nm[i]);
            }
        }
        if (!e && list)
            e = inet_printf("\n");
        freeifaddrs(ifs);
        return e;
    }

    char lname[16];
    const char *want = a->argv[o.ind++];
    if (inet_linux_ifname(want, lname)) {
        freeifaddrs(ifs);
        return inet_fail(&t, 1, "interface %s does not exist", want);
    }
    if (o.ind >= a->argc) {                                  /* one interface */
        e = status(ifs, lname);
        freeifaddrs(ifs);
        return e;
    }
    freeifaddrs(ifs);

#ifndef __linux__
    return inet_fail(&t, 1, "not available on this host: ROSGD's IfConfig sets Linux's interfaces");
#else
    struct ifreq r;
    int bring = 0, flags_set = 0, flags_clr = 0;
    for (int i = o.ind; !e && i < a->argc; i++) {
        const char *w = a->argv[i], *v = i + 1 < a->argc ? a->argv[i + 1] : NULL;
        struct in_addr x;
        if (!strcmp(w, "inet")) {
            continue;
        } else if (!strcmp(w, "up")) {
            flags_set |= IFF_UP;
        } else if (!strcmp(w, "down")) {
            flags_clr |= IFF_UP;
        } else if (!strcmp(w, "arp")) {
            flags_clr |= IFF_NOARP;
        } else if (!strcmp(w, "-arp")) {
            flags_set |= IFF_NOARP;
        } else if (!strcmp(w, "debug")) {
            flags_set |= IFF_DEBUG;
        } else if (!strcmp(w, "-debug")) {
            flags_clr |= IFF_DEBUG;
        } else if (!strcmp(w, "netmask") && v) {
            if (parse_in(v, &x))
                return inet_fail(&t, 1, "%s: bad value", v);
            e = set_addr(&t, SIOCSIFNETMASK, lname, x, "SIOCSIFNETMASK");
            i++;
        } else if (!strcmp(w, "broadcast") && v) {
            if (parse_in(v, &x))
                return inet_fail(&t, 1, "%s: bad value", v);
            e = set_addr(&t, SIOCSIFBRDADDR, lname, x, "SIOCSIFBRDADDR");
            i++;
        } else if (!strcmp(w, "mtu") && v) {
            memset(&r, 0, sizeof r);
            r.ifr_mtu = atoi(v);
            if (ifr_ioctl(SIOCSIFMTU, lname, &r) < 0)
                e = inet_fail(&t, 1, "ioctl (set mtu): %s", strerror(errno));
            i++;
        } else if (!strcmp(w, "metric") && v) {
            i++;                                    /* Linux keeps none: accepted, as 0 */
        } else if (parse_in(w, &x) == 0) {
            e = set_addr(&t, SIOCSIFADDR, lname, x, "SIOCSIFADDR");
            bring = 1;
        } else {
            return inet_fail(&t, 1, "%s: bad value", w);
        }
    }
    if (!e && (bring || flags_set || flags_clr)) {
        memset(&r, 0, sizeof r);
        if (ifr_ioctl(SIOCGIFFLAGS, lname, &r) < 0)
            return inet_fail(&t, 1, "ioctl (SIOCGIFFLAGS): %s", strerror(errno));
        short f = r.ifr_flags;
        f = (short)((f | flags_set | (bring && !(flags_clr & IFF_UP) ? IFF_UP : 0)) & ~flags_clr);
        if (f != r.ifr_flags) {
            r.ifr_flags = f;
            if (ifr_ioctl(SIOCSIFFLAGS, lname, &r) < 0)
                e = inet_fail(&t, 1, "ioctl (SIOCSIFFLAGS): %s", strerror(errno));
        }
    }
    return e;
#endif
}
