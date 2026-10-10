/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1984, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Sun Microsystems, Inc.
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
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's arp(8) as
 * ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/arp: c.ARP).
 */

/* arp.c -- *ARP (InetRes/Sources/arp, FreeBSD's arp), over Linux's ARP
 * table: /proc/net/arp to read it, SIOCSARP and SIOCDARP to change it.
 *
 * Entries print as FreeBSD's print_entry prints them:
 *   "host (1.2.3.4) at 52:54:00:12:34:56 on eth0 permanent [ethernet]"
 * Linux keeps no expiry time to show, so " expires in N seconds" is not
 * printed. An incomplete entry is "(incomplete)". ARP has no -e. */
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
#include <net/if_arp.h>
#endif

#include "inetres.h"

static const struct inet_tool tool = { "ARP", 0 };

#ifdef __linux__
static void name_of(struct in_addr a, int numeric, char *out, size_t max)
{
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_addr = a };
    if (numeric || getnameinfo((struct sockaddr *)&sin, sizeof sin, out, max, NULL, 0, NI_NAMEREQD))
        snprintf(out, max, "?");
}

static int resolve(const char *host, struct in_addr *a)
{
    struct addrinfo hints = { .ai_family = AF_INET }, *res;
    if (getaddrinfo(host, NULL, &hints, &res))
        return -1;
    *a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* Each entry of /proc/net/arp that matches, printed; how many matched. */
static int show(const char *want_if, const struct in_addr *only, int numeric, os_error **e)
{
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f)
        return 0;
    char line[256];
    int n = 0;
    if (!fgets(line, sizeof line, f)) {             /* the heading */
        fclose(f);
        return 0;
    }
    while (!*e && fgets(line, sizeof line, f)) {
        char ip[64], mac[64], mask[64], dev[32];
        unsigned type, flags;
        if (sscanf(line, "%63s 0x%x 0x%x %63s %63s %31s", ip, &type, &flags, mac, mask, dev) != 6)
            continue;
        struct in_addr a;
        inet_aton(ip, &a);
        char rdev[16];
        inet_riscos_ifname(dev, rdev);
        if ((only && a.s_addr != only->s_addr) || (want_if && strcmp(want_if, rdev) && strcmp(want_if, dev)))
            continue;
        char host[NI_MAXHOST];
        name_of(a, numeric, host, sizeof host);
        *e = inet_printf("%s (%s) at ", host, ip);
        if (!*e)
            *e = inet_printf("%s", (flags & ATF_COM) ? mac : "(incomplete)");
        if (!*e)
            *e = inet_printf(" on %s", rdev);
        if (!*e && (flags & ATF_PERM))
            *e = inet_printf(" permanent");
        if (!*e && (flags & ATF_PUBL))
            *e = inet_printf(" published");
        if (!*e && type == 1)
            *e = inet_printf(" [ethernet]");
        if (!*e)
            *e = inet_printf("\n");
        n++;
    }
    fclose(f);
    return n;
}

static os_error *change(int del, struct in_addr a, const char *ether, int temp, int pub, const char *ifname)
{
    struct arpreq r;
    memset(&r, 0, sizeof r);
    struct sockaddr_in *sin = (struct sockaddr_in *)&r.arp_pa;
    sin->sin_family = AF_INET;
    sin->sin_addr = a;
    if (ifname)
        snprintf(r.arp_dev, sizeof r.arp_dev, "%s", ifname);
    if (!del) {
        unsigned b[6];
        if (sscanf(ether, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
            return inet_fail(&tool, 1, "invalid Ethernet address '%s'", ether);
        r.arp_ha.sa_family = ARPHRD_ETHER;
        for (int i = 0; i < 6; i++)
            r.arp_ha.sa_data[i] = (char)b[i];
        r.arp_flags = ATF_COM | (temp ? 0 : ATF_PERM) | (pub ? ATF_PUBL : 0);
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int rc = s < 0 ? -1 : ioctl(s, del ? SIOCDARP : SIOCSARP, &r);
    int err = errno;
    if (s >= 0)
        close(s);
    if (rc < 0)
        return inet_fail(&tool, 1, "%s: %s", del ? "delete" : "set", strerror(err));
    if (del)
        return inet_printf("%s (%s) deleted\n", inet_ntoa(a), inet_ntoa(a));
    return NULL;
}
#endif

os_error *inet_arp(const struct inet_args *a)
{
#ifndef __linux__
    (void)a;
    return inet_fail(&tool, 1, "not available on this host: ROSGD's ARP reads Linux's table");
#else
    struct inet_opt o = { 0 };
    int numeric = 0, all = 0, del = 0, set = 0, c;
    const char *ifname = NULL;
    while ((c = inet_getopt(&o, a, "ani:dsSf:")) != -1) {
        switch (c) {
        case 'a':
            all = 1;
            break;
        case 'n':
            numeric = 1;
            break;
        case 'i':
            ifname = o.arg;
            break;
        case 'd':
            del = 1;
            break;
        case 's':
        case 'S':
            set = 1;
            break;
        case 'f':
            return inet_fail(&tool, 1, "-f is not carried by ROSGD's ARP");
        default:
            return inet_printf("Usage:   arp [-n] [-i interface] hostname\n"
                               "         arp [-n] [-i interface] -a\n"
                               "         arp -d hostname [pub]\n"
                               "         arp -s hostname ether_addr [temp] [pub]\n");
        }
    }
    char lif[16];
    const char *linux_if = NULL;
    if (ifname) {
        if (inet_linux_ifname(ifname, lif))
            return inet_fail(&tool, 1, "interface %s does not exist", ifname);
        linux_if = lif;
    }
    os_error *e = NULL;
    if (set) {
        if (a->argc - o.ind < 2)
            return inet_printf("Usage:   arp -s hostname ether_addr [temp] [pub]\n");
        struct in_addr ip;
        if (resolve(a->argv[o.ind], &ip))
            return inet_fail(&tool, 1, "%s: host not found", a->argv[o.ind]);
        int temp = 0, pub = 0;
        for (int i = o.ind + 2; i < a->argc; i++) {
            temp |= !strcmp(a->argv[i], "temp");
            pub |= !strcmp(a->argv[i], "pub");
        }
        return change(0, ip, a->argv[o.ind + 1], temp, pub, linux_if);
    }
    if (del) {
        if (all) {                                  /* every entry */
            FILE *f = fopen("/proc/net/arp", "r");
            char line[256], ip[64], dev[32], rest[128];
            if (f && fgets(line, sizeof line, f))
                while (!e && fgets(line, sizeof line, f))
                    if (sscanf(line, "%63s %*s %*s %127s %*s %31s", ip, rest, dev) == 3 &&
                        (!linux_if || !strcmp(dev, linux_if))) {
                        struct in_addr x;
                        inet_aton(ip, &x);
                        e = change(1, x, NULL, 0, 0, dev);
                    }
            if (f)
                fclose(f);
            return e;
        }
        if (o.ind >= a->argc)
            return inet_printf("Usage:   arp -d hostname [pub]\n");
        struct in_addr ip;
        if (resolve(a->argv[o.ind], &ip))
            return inet_fail(&tool, 1, "%s: host not found", a->argv[o.ind]);
        return change(1, ip, NULL, 0, 0, linux_if);
    }
    if (all) {
        show(ifname, NULL, numeric, &e);
        return e;
    }
    if (o.ind >= a->argc)
        return inet_printf("Usage:   arp [-n] [-i interface] hostname\n"
                           "         arp [-n] [-i interface] -a\n");
    struct in_addr ip;
    const char *host = a->argv[o.ind];
    if (resolve(host, &ip))
        return inet_fail(&tool, 1, "%s: host not found", host);
    if (!show(ifname, &ip, numeric, &e) && !e) {
        e = inet_printf("%s (%s) -- no entry", host, inet_ntoa(ip));
        if (!e && ifname)
            e = inet_printf(" on %s", ifname);
        if (!e)
            e = inet_printf("\n");
    }
    return e;
#endif
}
