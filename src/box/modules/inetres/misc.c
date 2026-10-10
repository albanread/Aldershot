/* Copyright 1999 Pace Micro Technology plc
 * Copyright 2003 Tematic Ltd
 * Copyright 1998 Acorn Computers Ltd
 * Copyright 1999 Element 14 Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation for BOX, over Linux, of RISC OS Open's
 * *IPVars, *GetHost, *IfRConfig and *ShowStat
 * (Sources/SystemRes/InetRes/Sources: ipvars/c.ipvars, gethost/c.gethost,
 * ifrconfig/c.IfRConfig, showstat/c.main and c.msgs).
 */

/* misc.c -- the !Internet commands of RISC OS's own: *IPVars, *GetHost,
 * *IfRConfig and *ShowStat (InetRes/Sources/ipvars, gethost, ifrconfig,
 * showstat), over Linux.
 *
 *   IPVars    Inet$<if>$Addr, $Mask, $Network, $Host, $Broadcast for each
 *             interface with an address, and $MAC as link_ntoa writes it
 *             ("b8.27.eb.1.2.3": lower case, no leading zeros, dots).
 *   GetHost   [-t] [-x] <host> ...: "Hostname:", "Alias:", "Address:",
 *             "type:", "length:", and a blank line after each.
 *             Sys$ReturnCode is 2 for a bad address and 3 for a failed
 *             lookup.
 *   IfRConfig <if> [revarp|bootp] [netmask]: the interface's status. With
 *             a keyword there is nothing to do where Linux has given the
 *             interface an address (the original acts only on one with
 *             none). Then Inet$HostName is set if unset, from the
 *             address's name, else "ARM%08lx". RARP and BOOTP themselves
 *             are Linux's (ip=dhcp) and are not carried.
 *   ShowStat  the DCI4 statistics block for each Ethernet interface, from
 *             Linux's /sys/class/net: "%-22s: %s" lines, with the counters
 *             shown when non-zero (TX and RX frames always, -v all). */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/if_packet.h>
#else
#include <net/if_dl.h>
#endif

#include "inetres.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

static void set_var(const char *name, const char *value)
{
    size_t nl = strlen(name) + 1, vl = strlen(value);
    char *b = ros_rma_alloc(nl + vl + 1);
    if (!b)
        return;
    memcpy(b, name, nl);
    memcpy(b + nl, value, vl + 1);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(b), s.r[1] = ros_addr(b + nl), s.r[2] = (uint32_t)vl, s.r[3] = 0, s.r[4] = 0;
    ros_swi(&s, ROS_X_BIT | 0x24);
    ros_rma_free(b);
}

/* A variable's value, "" if unset. */
static void get_var(const char *name, char *out, size_t max)
{
    size_t nl = strlen(name) + 1;
    char *b = ros_rma_alloc(nl + max);
    out[0] = 0;
    if (!b)
        return;
    memcpy(b, name, nl);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(b), s.r[1] = ros_addr(b + nl), s.r[2] = (uint32_t)(max - 1), s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, ROS_X_BIT | 0x23);                  /* OS_ReadVarVal, expanded */
    if (!s.v) {
        size_t n = s.r[2] < max - 1 ? s.r[2] : max - 1;
        memcpy(out, b + nl, n);
        out[n] = 0;
    }
    ros_rma_free(b);
}

static const unsigned char *mac_of(struct ifaddrs *x)
{
    if (!x->ifa_addr)
        return NULL;
#ifdef __linux__
    if (x->ifa_addr->sa_family == AF_PACKET) {
        struct sockaddr_ll *ll = (struct sockaddr_ll *)x->ifa_addr;
        return ll->sll_halen == 6 ? ll->sll_addr : NULL;
    }
#else
    if (x->ifa_addr->sa_family == AF_LINK) {
        struct sockaddr_dl *dl = (struct sockaddr_dl *)x->ifa_addr;
        return dl->sdl_alen == 6 ? (const unsigned char *)LLADDR(dl) : NULL;
    }
#endif
    return NULL;
}

/* ---- IPVars ---------------------------------------------------------------------- */

os_error *inet_ipvars(const struct inet_args *a)
{
    if (a->argc > 1 && !strcmp(a->argv[1], "-help"))
        return inet_printf("Usage:   ipvars\n         sets up RISC OS system variables for each interface\n");
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0) {
        inet_exit(1);
        return inet_printf("socket() failed -> errno %d (%s)\n", errno, strerror(errno));
    }
    for (struct ifaddrs *x = all; x; x = x->ifa_next) {
        char rif[16], name[64], v[32];
        inet_riscos_ifname(x->ifa_name, rif);
        const unsigned char *mac = mac_of(x);
        if (mac && !(x->ifa_flags & IFF_LOOPBACK)) {
            snprintf(name, sizeof name, "Inet$%s$MAC", rif);
            snprintf(v, sizeof v, "%x.%x.%x.%x.%x.%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            set_var(name, v);
        }
        if (!x->ifa_addr || x->ifa_addr->sa_family != AF_INET)
            continue;
        uint32_t ad = ntohl(((struct sockaddr_in *)x->ifa_addr)->sin_addr.s_addr);
        uint32_t m = x->ifa_netmask ? ntohl(((struct sockaddr_in *)x->ifa_netmask)->sin_addr.s_addr) : 0xFFFFFFFFu;
        static const char *const what[] = { "Addr", "Mask", "Network", "Host", "Broadcast" };
        uint32_t val[5] = { ad, m, ad & m, ad & ~m, (ad & m) | ~m };
        for (int i = 0; i < 5; i++) {
            struct in_addr in = { htonl(val[i]) };
            snprintf(name, sizeof name, "Inet$%s$%s", rif, what[i]);
            set_var(name, inet_ntoa(in));
        }
    }
    freeifaddrs(all);
    return NULL;
}

/* ---- GetHost --------------------------------------------------------------------- */

static os_error *gethost_help(int code)
{
    inet_exit(code);
    return inet_printf("Usage:   gethost [-t] [-x] <hostname>\n"
                       "         the gethost utility does hostname and IP lookups\n"
                       "Options: -t  show the elapsed time of the lookup in fractional seconds\n"
                       "         -x  do a reverse lookup; find a hostname from an IP address\n");
}

os_error *inet_gethost(const struct inet_args *a)
{
    if (a->argc < 2)
        return gethost_help(1);
    if (!strcmp(a->argv[1], "-help"))
        return gethost_help(0);
    int timing = 0, reverse = 0, rc = 0;
    os_error *e = NULL;
    for (int i = 1; !e && i < a->argc; i++) {
        const char *w = a->argv[i];
        if (w[0] == '-') {
            for (const char *p = w + 1; *p; p++) {
                if (*p == 't')
                    timing = 1;
                else if (*p == 'x')
                    reverse = 1;
                else {
                    inet_exit(1);
                    return inet_printf("gethost: illegal option\n");
                }
            }
            continue;
        }
        if (strchr(w, ':'))
            reverse = 1;
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        struct hostent *h;
        if (reverse) {
            struct in_addr in;
            if (!inet_aton(w, &in)) {
                e = inet_printf("Invalid address \"%s\"\n\n", w);
                rc = 2;
                timing = reverse = 0;
                continue;
            }
            h = gethostbyaddr((const char *)&in, sizeof in, AF_INET);
        } else {
            h = gethostbyname(w);
        }
        gettimeofday(&t1, NULL);
        if (!h) {
            e = inet_printf("Failed to look up \"%s\"\n", w);
            rc = 3;
        } else {
            e = inet_printf("Hostname: %s\n", h->h_name);
            for (char **al = h->h_aliases; !e && al && *al; al++)
                e = inet_printf("Alias:    %s\n", *al);
            for (char **ad = h->h_addr_list; !e && ad && *ad; ad++) {
                char text[64];
                inet_ntop(h->h_addrtype, *ad, text, sizeof text);
                e = inet_printf("Address:  %s\n   type:  %d\n length:  %d\n", text, h->h_addrtype == AF_INET ? 2 : h->h_addrtype,
                                h->h_length);
            }
            rc = 0;
        }
        if (!e && timing)
            e = inet_printf("%s() took %.3f seconds\n", reverse ? "gethostbyaddr" : "gethostbyname",
                            (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6);
        if (!e)
            e = inet_printf("\n");
        timing = reverse = 0;
    }
    inet_exit(rc);
    return e;
}

/* ---- IfRConfig ------------------------------------------------------------------- */

os_error *inet_ifrconfig(const struct inet_args *a)
{
    int i = 1, e_flag = 0;
    if (i < a->argc && !strcmp(a->argv[i], "-help")) {
        return inet_printf("Usage:   ifrconfig [-e] <interface> [ revarp | bootp ] [ netmask ]\n"
                           "         configure network interface address and netmask via Reverse ARP or BOOTP\n"
                           "Options: -e  write any errors to Inet$Error\n");
    }
    if (i < a->argc && !strcmp(a->argv[i], "-e"))
        e_flag = 1, i++;
    if (i >= a->argc) {
        inet_exit(1);
        return inet_printf("Usage:   ifrconfig [-e] <interface> [ revarp | bootp ] [ netmask ]\n");
    }
    const char *ifname = a->argv[i++];
    char lname[16], text[256];
    struct ifaddrs *all, *hit = NULL, *any = NULL;
    if (inet_linux_ifname(ifname, lname) || getifaddrs(&all) < 0) {
        snprintf(text, sizeof text, "ifrconfig error when accessing interface %s: interface name not known", ifname);
        inet_exit(1);
        if (e_flag) {
            set_var("Inet$Error", text);
            return NULL;
        }
        return inet_printf("%s\n", text);
    }
    for (struct ifaddrs *x = all; x; x = x->ifa_next)
        if (!strcmp(x->ifa_name, lname)) {
            any = x;
            if (x->ifa_addr && x->ifa_addr->sa_family == AF_INET)
                hit = x;
        }
    os_error *e = NULL;
    if (i >= a->argc) {                                     /* the status */
        char rif[16];
        inet_riscos_ifname(lname, rif);
        unsigned f = any ? any->ifa_flags : 0;
        e = inet_printf("%s: flags=%x<%s%s%s%s>\n", rif, f & 0x7F, f & IFF_UP ? "UP" : "",
                        f & IFF_BROADCAST ? ",BROADCAST" : "", f & IFF_LOOPBACK ? ",LOOPBACK" : "",
                        f & IFF_RUNNING ? ",RUNNING" : "");
        if (!e && hit) {
            uint32_t m = hit->ifa_netmask ? ntohl(((struct sockaddr_in *)hit->ifa_netmask)->sin_addr.s_addr) : 0;
            e = inet_printf("\tinet %s netmask %lx ", inet_ntoa(((struct sockaddr_in *)hit->ifa_addr)->sin_addr),
                            (unsigned long)m);
            if (!e && (hit->ifa_flags & IFF_BROADCAST) && hit->ifa_broadaddr)
                e = inet_printf("broadcast %s", inet_ntoa(((struct sockaddr_in *)hit->ifa_broadaddr)->sin_addr));
            if (!e)
                e = inet_printf("\n");
        }
        freeifaddrs(all);
        return e;
    }
    if (!hit) {
        freeifaddrs(all);
        snprintf(text, sizeof text,
                 "ifrconfig error when accessing interface %s: no address -- Reverse ARP and BOOTP are Linux's "
                 "(ip=dhcp) under ROSGD", ifname);
        inet_exit(1);
        if (e_flag) {
            set_var("Inet$Error", text);
            return NULL;
        }
        return inet_printf("%s\n", text);
    }
    char host[256];
    get_var("Inet$HostName", host, sizeof host);
    if (!host[0]) {
        struct sockaddr_in *sin = (struct sockaddr_in *)hit->ifa_addr;
        char name[NI_MAXHOST];
        if (!getnameinfo((struct sockaddr *)sin, sizeof *sin, name, sizeof name, NULL, 0, NI_NAMEREQD)) {
            char *dot = strchr(name, '.');
            if (dot)
                *dot = 0;
            set_var("Inet$HostName", name);
        } else {
            char arm[16];
            snprintf(arm, sizeof arm, "ARM%08lx", (unsigned long)ntohl(sin->sin_addr.s_addr));
            set_var("Inet$HostName", arm);
        }
    }
    freeifaddrs(all);
    return NULL;
}

/* ---- ShowStat -------------------------------------------------------------------- */

#ifdef __linux__
#define WIDTH 22

static int sysfs(const char *ifname, const char *leaf, char *out, size_t max)
{
    char path[160];
    snprintf(path, sizeof path, "/sys/class/net/%s/%s", ifname, leaf);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)max, f))
        out[0] = 0;
    out[strcspn(out, "\n")] = 0;
    fclose(f);
    return 0;
}

static os_error *line(const char *label, const char *value)
{
    return *value ? inet_printf("%-*s: %s\n", WIDTH, label, value) : NULL;
}
#endif

os_error *inet_showstat(const struct inet_args *a)
{
    int verbose = 0;
    const char *only[8];
    int nonly = 0;
    for (int i = 1; i < a->argc; i++) {
        if (!strcmp(a->argv[i], "-help"))
            return inet_printf("DCI4 Statistics Display 1.00 (26 Sep 2026)\n\n"
                               "Usage:   showstat [-help] [-v] [-type <type>] [[<device>|<interface>] ...]\n");
        else if (!strcmp(a->argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(a->argv[i], "-file"))
            return inet_printf("showstat: -file is not carried by ROSGD's ShowStat\n");
        else if (!strcmp(a->argv[i], "-type"))
            i++;                                            /* no extended statistics providers */
        else if (a->argv[i][0] != '-' && nonly < 8)
            only[nonly++] = a->argv[i];
    }
    os_error *e = inet_printf("DCI4 Statistics Display 1.00 (26 Sep 2026)\n");
#ifdef __linux__
    struct ifaddrs *all;
    if (e || getifaddrs(&all) < 0)
        return e;
    char seen[16][16];
    int nseen = 0;
    for (struct ifaddrs *x = all; !e && x; x = x->ifa_next) {
        if ((x->ifa_flags & IFF_LOOPBACK) || !mac_of(x) || nseen == 16)
            continue;
        int dup = 0;
        for (int k = 0; k < nseen; k++)
            dup |= !strcmp(seen[k], x->ifa_name);
        if (dup)
            continue;
        snprintf(seen[nseen++], 16, "%s", x->ifa_name);
        char name[16];
        size_t l = strcspn(x->ifa_name, "0123456789");
        snprintf(name, sizeof name, "%.*s", (int)l, x->ifa_name);
        int unit = atoi(x->ifa_name + l);
        if (nonly) {
            int want = 0;
            for (int k = 0; k < nonly; k++)
                want |= !strcmp(only[k], x->ifa_name) || !strcmp(only[k], name);
            if (!want)
                continue;
        }
        const unsigned char *m = mac_of(x);
        char v[160], mac[32], driver[64] = "", speed[16] = "", duplex[16] = "", carrier[8] = "", oper[16] = "";
        snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        char link[256], path[160];
        snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", x->ifa_name);
        ssize_t n = readlink(path, link, sizeof link - 1);
        if (n > 0) {
            link[n] = 0;
            char *slash = strrchr(link, '/');
            snprintf(driver, sizeof driver, "%s", slash ? slash + 1 : link);
        }
        sysfs(x->ifa_name, "speed", speed, sizeof speed);
        sysfs(x->ifa_name, "duplex", duplex, sizeof duplex);
        sysfs(x->ifa_name, "carrier", carrier, sizeof carrier);
        sysfs(x->ifa_name, "operstate", oper, sizeof oper);
        e = inet_printf("\n");
        if (!e)
            e = line("Interface name", name);
        snprintf(v, sizeof v, "%d", unit);
        if (!e)
            e = line("Unit number", v);
        if (!e)
            e = line("Hardware address", mac);
        if (!e)
            e = line("Location", "Linux");
        if (!e)
            e = line("Driver module", driver);
        if (!e && sysfs(x->ifa_name, "mtu", v, sizeof v) == 0)
            e = line("MTU", v);
        int sp = atoi(speed);
        if (!e && sp > 0)
            e = line("Interface type", sp >= 1000 ? "1000baseT" : sp >= 100 ? "100baseTX" : "10baseT");
        if (!e && carrier[0])
            e = line("Link status", carrier[0] == '1' ? "Interface OK" : "Interface faulty");
        if (!e && oper[0])
            e = line("Active status", !strcmp(oper, "up") ? "Interface is active" : "Interface is inactive");
        if (!e)
            e = line("Receive mode", (x->ifa_flags & IFF_PROMISC) ? "Direct, broadcast, multicast and promiscuous"
                                     : (x->ifa_flags & IFF_MULTICAST) ? "Direct, broadcast and multicast"
                                     : (x->ifa_flags & IFF_BROADCAST) ? "Direct and broadcast" : "Direct");
        if (!e && (!strcmp(duplex, "full") || !strcmp(duplex, "half")))
            e = line("Interface mode", duplex[0] == 'f' ? "Full duplex" : "Half-duplex");
        static const struct { int always; const char *label, *leaf; } counters[] = {
            { 0, "Network collisions", "statistics/collisions" },
            { 0, "TX heartbeat failures", "statistics/tx_heartbeat_errors" },
            { 1, "TX frames", "statistics/tx_packets" },
            { 0, "TX bytes", "statistics/tx_bytes" },
            { 0, "TX general errors", "statistics/tx_errors" },
            { 0, "RX CRC failures", "statistics/rx_crc_errors" },
            { 0, "RX alignment errors", "statistics/rx_frame_errors" },
            { 0, "RX dropped frames", "statistics/rx_dropped" },
            { 0, "RX overlong frames", "statistics/rx_length_errors" },
            { 1, "RX frames", "statistics/rx_packets" },
            { 0, "RX bytes", "statistics/rx_bytes" },
            { 0, "RX general errors", "statistics/rx_errors" },
        };
        for (unsigned k = 0; !e && k < sizeof counters / sizeof counters[0]; k++) {
            if (sysfs(x->ifa_name, counters[k].leaf, v, sizeof v))
                continue;
            if (counters[k].always || verbose || strtoull(v, NULL, 10))
                e = line(counters[k].label, v);
        }
    }
    freeifaddrs(all);
#else
    (void)verbose;
#endif
    return e;
}
