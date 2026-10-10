/* Copyright RISC OS Open Ltd and others
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
 * This file is derived from RISC OS Open's source and from other code.
 * The Apache licence of RISC OS Open's source applies to it.
 */

/* dhcp.c -- DHCP, rewritten for ROSGD as a native module over Linux.
 *
 * RISC OS 5's DHCP module (0.28, Sources/Networking/DHCP) is a DHCP client.
 * *DHCPExecute hands it an interface and it runs the protocol. When the
 * interface is bound, the Internet module sets the Inet$ variables from the
 * server's DHCPACK (Internet's whoami.c, bootp_interpret). !Internet's
 * Startup runs *DHCPExecute -e -b and carries on from those variables.
 *
 * Under ROSGD the network stack is Linux's, and the kernel is the DHCP
 * client. ip=dhcp on its command line brings the interface up with a lease
 * before /init runs, and /proc/net/pnp records the name servers, the
 * domain and the server. So this module runs no protocol. It is the
 * interface over the kernel's lease:
 *
 *   - An interface is BOUND when it has its IPv4 address, and INIT when it
 *     has none. It is named as Linux names it, or as RISC OS does (eth0,
 *     lo0, a driver's own name and unit), as the !Internet commands take
 *     the names (inetres.h).
 *   - The lease is a DHCPACK, built from what Linux knows: the address and
 *     netmask of the interface, the default route's gateway, and from
 *     /proc/net/pnp the name servers, the domain and the server. GetState
 *     and GetOption read that packet as the original reads the server's.
 *   - *DHCPExecute, and DHCP_Execute, on a bound interface set the Inet$
 *     variables as whoami's bootp_interpret sets them from a DHCPACK.
 *   - -b blocks until the interface has an address, and -w until it
 *     exists. Both give way to Escape. -p gives an interface without an
 *     address a private one, 169.254.a.b from its MAC, as the original
 *     does on ABANDON.
 *   - What the kernel owns is not pretended. Renewal is the kernel's, and
 *     its lease is held for as long as the machine runs. An interface's
 *     request options are the kernel's to send, so SetOption on an
 *     interface is "Option not added", though on a packet it works.
 *     DHCPINFORM is not sent.
 *
 * The hosted build has the interfaces and their addresses, and nothing of
 * the host's DHCP. Its leases are the address options alone.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <netpacket/packet.h>
#else
#include <net/if_dl.h>
#endif

#include "dhcp.h"
#include "inetres.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define VERSION    28           /* DHCP 0.28 */
#define ERROR_BASE 0x816C00u    /* dhcperror_BASE */
enum { E_NO_SUCH_INTERFACE, E_BAD_OPTION, E_OPTION_NOT_PRESENT, E_OPTION_NOT_ADDED };

/* dhcp_state, h.DHCP's numbering */
enum { S_INIT, S_SELECTING, S_REQUESTING, S_BOUND, S_RENEWING, S_REBINDING, S_INITREBOOT,
       S_REBOOTING, S_ABANDON };

/* DHCP_Execute's flags, and *DHCPExecute's */
#define DSE_BLOCK   4u
#define DSE_PRIVATE 8u
#define DECF_ERROR  1u
#define DECF_BLOCK  2u
#define DECF_WAIT   4u
#define DECF_PRIVATE 8u

/* ---- the packet (TCPIPLibs' protocols/dhcp.h: BOOTP) ----------------------- */

#define PACKET      548         /* 236 bytes of header, 312 of options */
#define OPTIONS     236
#define COOKIE      "\x63\x82\x53\x63"
enum { O_PAD = 0, O_NETMASK = 1, O_ROUTER = 3, O_NAMESERVER = 6, O_HOSTNAME = 12, O_DOMAINNAME = 15,
       O_BROADCAST = 28, O_MESSAGETYPE = 53, O_SERVERID = 54, O_EXTENSION = 127, O_END = 255 };
#define DHCPACK 5

/* The option's code byte in a packet, or NULL. */
static uint8_t *find_option(uint8_t *p, unsigned id)
{
    if (memcmp(p + OPTIONS, COOKIE, 4))
        return NULL;
    for (size_t i = OPTIONS + 4; i < PACKET && p[i] != O_END;) {
        if (p[i] == O_PAD) {
            i++;
            continue;
        }
        if (i + 1 >= PACKET || i + 2 + p[i + 1] > PACKET)
            return NULL;
        if (p[i] == id)
            return p + i;
        i += 2 + p[i + 1];
    }
    return NULL;
}

/* Where O_END stands, or NULL in a packet without the cookie. */
static uint8_t *end_of(uint8_t *p)
{
    if (memcmp(p + OPTIONS, COOKIE, 4))
        return NULL;
    size_t i = OPTIONS + 4;
    while (i < PACKET && p[i] != O_END)
        i += p[i] == O_PAD ? 1 : 2u + (i + 1 < PACKET ? p[i + 1] : 0);
    return i < PACKET ? p + i : NULL;
}

static void delete_option(uint8_t *p, unsigned id)
{
    uint8_t *o = find_option(p, id), *end = end_of(p);
    if (!o || !end)
        return;
    size_t len = 2u + o[1];
    memmove(o, o + len, (size_t)(end + 1 - (o + len)));
    memset(end + 1 - len, O_PAD, len);
}

static int add_option(uint8_t *p, unsigned id, size_t len, const void *data)
{
    uint8_t *end = end_of(p);
    if (!end || len > 255 || end + 3 + len > p + PACKET)
        return -1;
    end[0] = (uint8_t)id;
    end[1] = (uint8_t)len;
    memcpy(end + 2, data, len);
    end[2 + len] = O_END;
    return 0;
}

/* ---- the kernel's lease ------------------------------------------------- */

struct lease {
    char linux_name[16], name[16];  /* Linux's, and as the caller named it */
    int state;
    uint8_t packet[PACKET];
};

/* The kernel's DHCP answer, /proc/net/pnp: "#PROTO: DHCP", then
 * "domain", "nameserver" and "bootserver" lines. */
struct pnp {
    int dhcp;
    char domain[64];
    uint8_t ns[3][4];
    int nns;
    uint8_t server[4];
    int has_server;
};

static void read_pnp(struct pnp *k)
{
    memset(k, 0, sizeof *k);
    FILE *f = fopen("/proc/net/pnp", "r");
    char line[256], word[64], value[128];
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "#PROTO: DHCP", 12))
            k->dhcp = 1;
        if (sscanf(line, "%63s %127s", word, value) != 2)
            continue;
        struct in_addr a;
        if (!strcmp(word, "domain"))
            snprintf(k->domain, sizeof k->domain, "%s", value);
        else if (!strcmp(word, "nameserver") && k->nns < 3 && inet_pton(AF_INET, value, &a) == 1 &&
                 a.s_addr)
            memcpy(k->ns[k->nns++], &a, 4);
        else if (!strcmp(word, "bootserver") && inet_pton(AF_INET, value, &a) == 1 && a.s_addr)
            memcpy(k->server, &a, 4), k->has_server = 1;
    }
    if (f)
        fclose(f);
}

/* The default route's gateway through an interface, from /proc/net/route. */
static int read_gateway(const char *linux_name, uint8_t out[4])
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], dev[32];
    unsigned dest, gw, flags;
    int found = 0;
    if (f && fgets(line, sizeof line, f))
        while (!found && fgets(line, sizeof line, f))
            if (sscanf(line, "%31s %x %x %x", dev, &dest, &gw, &flags) == 4 && !dest && gw &&
                !strcmp(dev, linux_name))
                memcpy(out, &gw, 4), found = 1;   /* already in network order */
    if (f)
        fclose(f);
    return found;
}

/* The lease on an interface: 0, or -1 if there is no such interface. */
static int read_lease(const char *name, struct lease *l)
{
    memset(l, 0, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name);
    if (inet_linux_ifname(name, l->linux_name))
        return -1;
    struct ifaddrs *all, *a;
    if (getifaddrs(&all) < 0)
        return -1;
    int exists = 0;
    struct sockaddr_in *addr = NULL, *mask = NULL, *broad = NULL;
    const uint8_t *mac = NULL;
    for (a = all; a; a = a->ifa_next) {
        if (strcmp(a->ifa_name, l->linux_name) || !a->ifa_addr)
            continue;
        exists = 1;
        if (a->ifa_addr->sa_family == AF_INET && !addr) {
            addr = (struct sockaddr_in *)a->ifa_addr;
            mask = (struct sockaddr_in *)a->ifa_netmask;
            broad = (a->ifa_flags & IFF_BROADCAST) ? (struct sockaddr_in *)a->ifa_broadaddr : NULL;
        }
#ifdef __linux__
        if (a->ifa_addr->sa_family == AF_PACKET && ((struct sockaddr_ll *)a->ifa_addr)->sll_halen == 6)
            mac = ((struct sockaddr_ll *)a->ifa_addr)->sll_addr;
#else
        if (a->ifa_addr->sa_family == AF_LINK && ((struct sockaddr_dl *)a->ifa_addr)->sdl_alen == 6)
            mac = (const uint8_t *)LLADDR((struct sockaddr_dl *)a->ifa_addr);
#endif
    }
    if (!exists && if_nametoindex(l->linux_name))
        exists = 1;
    if (!exists) {
        freeifaddrs(all);
        return -1;
    }

    uint8_t *p = l->packet;
    p[0] = 2, p[1] = 1, p[2] = 6;                   /* BOOTREPLY, Ethernet */
    uint32_t xid = htonl(if_nametoindex(l->linux_name));
    memcpy(p + 4, &xid, 4);
    if (mac)
        memcpy(p + 28, mac, 6);
    memcpy(p + OPTIONS, COOKIE, 4);
    p[OPTIONS + 4] = O_END;
    if (addr) {
        l->state = S_BOUND;
        memcpy(p + 16, &addr->sin_addr, 4);         /* yiaddr */
        uint8_t ack = DHCPACK;
        add_option(p, O_MESSAGETYPE, 1, &ack);
        struct pnp k;
        read_pnp(&k);
        if (k.dhcp && k.has_server) {
            memcpy(p + 20, k.server, 4);            /* siaddr */
            add_option(p, O_SERVERID, 4, k.server);
        }
        if (mask)
            add_option(p, O_NETMASK, 4, &mask->sin_addr);
        uint8_t gw[4];
        if (read_gateway(l->linux_name, gw))
            add_option(p, O_ROUTER, 4, gw);
        if (k.nns)
            add_option(p, O_NAMESERVER, 4u * (unsigned)k.nns, k.ns);
        /* The kernel names the host from the lease's option 12, and after
         * its own address when there is none: that is not a host name. */
        struct utsname u;
        struct in_addr dotted;
        if (k.dhcp && uname(&u) == 0 && u.nodename[0] && strcmp(u.nodename, "(none)") &&
            inet_pton(AF_INET, u.nodename, &dotted) != 1)
            add_option(p, O_HOSTNAME, strlen(u.nodename), u.nodename);
        if (k.domain[0])
            add_option(p, O_DOMAINNAME, strlen(k.domain), k.domain);
        if (broad)
            add_option(p, O_BROADCAST, 4, &broad->sin_addr);
    } else {
        l->state = S_INIT;
    }
    freeifaddrs(all);
    return 0;
}

static os_error *error(unsigned n)
{
    static const char *const text[] = { "No such interface under DHCP control",
                                        "Illegal option (must be 1..254)", "Option not present",
                                        "Option not added" };
    return ros_error(ERROR_BASE + n, "%s", text[n]);
}

/* ---- the Inet$ variables, as whoami's bootp_interpret sets them ------------------ */

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
    ros_swi(&s, ROS_X_BIT | 0x24);                  /* OS_SetVarVal, a string */
    ros_rma_free(b);
}

static void set_addr_var(const char *name, const uint8_t *a)
{
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, a, text, sizeof text);
    set_var(name, text);
}

static void set_vars(struct lease *l)
{
    uint8_t *p = l->packet, *o;
    char text[256];
    set_addr_var("Inet$EtherIPAddr", p + 16);
    set_addr_var("Inet$BootServer", p + 20);
    set_var("Inet$BootFile", "");
    if ((o = find_option(p, O_HOSTNAME))) {
        snprintf(text, sizeof text, "%.*s", o[1], (const char *)o + 2);
        set_var("Inet$HostName", text);
    }
    if ((o = find_option(p, O_DOMAINNAME))) {
        snprintf(text, sizeof text, "%.*s", o[1], (const char *)o + 2);
        set_var("Inet$LocalDomain", text);
    }
    if ((o = find_option(p, O_NETMASK)) && o[1] == 4)
        set_addr_var("Inet$EtherIPMask", o + 2);
    if ((o = find_option(p, O_ROUTER)) && o[1] >= 4)
        set_addr_var("Inet$Gateway", o + 2);
    if ((o = find_option(p, O_NAMESERVER)) && o[1] && !(o[1] & 3)) {
        size_t n = 0;
        for (unsigned i = 0; i < o[1] / 4u && i < 3; i++) {
            char one[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, o + 2 + 4 * i, one, sizeof one);
            n += (size_t)snprintf(text + n, sizeof text - n, "%s%s", i ? " " : "", one);
        }
        set_var("Inet$Resolvers", text);
    }
}

/* ---- a private address, when -p and there is no lease ----------------------------- */

static int assign_private(struct lease *l)
{
#ifdef __linux__
    const uint8_t *mac = l->packet + 28;
    if (!mac[0] && !mac[1] && !mac[2] && !mac[3] && !mac[4] && !mac[5])
        return -1;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    struct ifreq r;
    struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;
    int rc = 0;
    uint32_t addr = htonl(0xA9FE0000u | (uint32_t)mac[4] << 8 | mac[5]), mask = htonl(0xFFFF0000u);
    uint32_t values[3] = { addr, mask, addr | ~mask };
    unsigned long ops[3] = { SIOCSIFADDR, SIOCSIFNETMASK, SIOCSIFBRDADDR };
    for (int i = 0; i < 3 && !rc; i++) {
        memset(&r, 0, sizeof r);
        snprintf(r.ifr_name, sizeof r.ifr_name, "%s", l->linux_name);
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = values[i];
        rc = ioctl(s, ops[i], &r);
    }
    memset(&r, 0, sizeof r);
    snprintf(r.ifr_name, sizeof r.ifr_name, "%s", l->linux_name);
    if (!rc && ioctl(s, SIOCGIFFLAGS, &r) == 0 && !(r.ifr_flags & IFF_UP)) {
        r.ifr_flags |= IFF_UP;
        rc = ioctl(s, SIOCSIFFLAGS, &r);
    }
    close(s);
    return rc;
#else
    (void)l;
    return -1;
#endif
}

/* ---- the SWIs ------------------------------------------------------------------- */

os_error *xdhcp_version(uint32_t *version)
{
    *version = VERSION;
    return NULL;
}

os_error *xdhcp_execute(uint32_t flags, const char *if_name, uint32_t *status)
{
    struct lease l;
    if (read_lease(if_name, &l))
        return error(E_NO_SUCH_INTERFACE);
    if (l.state == S_INIT && (flags & DSE_PRIVATE)) {
        assign_private(&l);
        *status = S_ABANDON;
        return NULL;
    }
    if (l.state == S_BOUND)
        set_vars(&l);
    *status = (uint32_t)l.state;
    return NULL;
}

os_error *xdhcp_get_state(uint32_t flags, const char *if_name, uint32_t buffer, int32_t size,
                          uint32_t *status, int32_t *size_left)
{
    (void)flags;
    struct lease l;
    if (read_lease(if_name, &l))
        return error(E_NO_SUCH_INTERFACE);
    *status = (uint32_t)l.state;
    if (buffer && size > 0)
        memcpy(ros_ptr(buffer), l.packet, size < PACKET ? (size_t)size : PACKET);
    *size_left = size - PACKET;
    return NULL;
}

os_error *xdhcp_get_option(uint32_t flags, uint32_t if_name, uint32_t option, uint32_t buffer,
                           int32_t size, uint32_t *status, int32_t *size_left)
{
    if (option == O_PAD || option == O_END || (option > 0xFFFF && option >> 16 != O_EXTENSION))
        return error(E_BAD_OPTION);
    struct lease l;
    uint8_t *p;
    if (flags & 1) {
        p = ros_ptr(if_name);
        *status = S_BOUND;
    } else {
        if (read_lease(ros_ptr(if_name), &l))
            return error(E_NO_SUCH_INTERFACE);
        p = l.packet;
        *status = (uint32_t)l.state;
    }
    uint8_t *o = option > 0xFF ? NULL : find_option(p, option);
    if (!o)
        return error(E_OPTION_NOT_PRESENT);
    if (buffer && size > 0)
        memcpy(ros_ptr(buffer), o + 2, size < o[1] ? (size_t)size : o[1]);
    *size_left = size - o[1];
    return NULL;
}

os_error *xdhcp_set_option(uint32_t flags, uint32_t if_name, uint32_t option, uint32_t buffer,
                           uint32_t size, uint32_t *status)
{
    if ((option & 0xFF) == O_PAD || (option & 0xFF) == O_END || size > 255)
        return error(E_BAD_OPTION);
    if (!(flags & 1)) {
        struct lease l;
        if (read_lease(ros_ptr(if_name), &l))
            return error(E_NO_SUCH_INTERFACE);
        *status = (uint32_t)l.state;
        return error(E_OPTION_NOT_ADDED);           /* the kernel's request */
    }
    uint8_t *p = ros_ptr(if_name);
    *status = S_BOUND;
    delete_option(p, option & 0xFF);
    if (buffer && add_option(p, option & 0xFF, size, ros_ptr(buffer)))
        return error(E_OPTION_NOT_ADDED);
    return NULL;
}

os_error *xdhcp_inform(uint32_t flags, uint32_t address, uint32_t req_list, uint32_t len_req_list)
{
    (void)flags, (void)address, (void)req_list, (void)len_req_list;
    return NULL;
}

/* ---- the commands --------------------------------------------------------------- */

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

/* "Interface     : eth0": the label padded past "---Longest---", as
 * interfaces_print_heading pads it. */
static os_error *heading(const char *label, char sep)
{
    return print("%-14s%c ", label, sep);
}

static os_error *info(struct lease *l)
{
    static const char *const states[] = { "INIT", "SELECTING", "REQUESTING", "BOUND", "RENEWING",
                                          "REBINDING", "INITREBOOT", "REBOOTING", "ABANDON" };
    os_error *e = print("\n");
    if (!e)
        e = heading("Interface", ':');
    if (!e)
        e = print("%s\n", l->name);
    if (!e)
        e = heading("State", ':');
    if (!e)
        e = print("%s\n", states[l->state]);
    if (!e && l->state == S_BOUND) {
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, l->packet + 16, text, sizeof text);
        e = heading("IP address", ':');
        if (!e)
            e = print("%s\n", text);
        uint8_t *o = find_option(l->packet, O_SERVERID);
        if (!e && o && o[1] == 4) {
            inet_ntop(AF_INET, o + 2, text, sizeof text);
            e = heading("DHCP server", ':');
            if (!e)
                e = print("%s\n", text);
        }
        if (!e)
            e = heading("Lease expires", '@');
        if (!e)
            e = print("Never (infinite lease)\n");
    }
    return e;
}

static os_error *cmd_dhcpinfo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    char name[16] = "";
    const char *t = ros_ptr(tail);
    while (*t == ' ')
        t++;
    size_t n = 0;
    while (t[n] > ' ' && n < sizeof name - 1)
        name[n] = t[n], n++;
    name[n] = 0;
    struct lease l;
    if (argc && name[0] && read_lease(name, &l))
        return error(E_NO_SUCH_INTERFACE);
    os_error *e = print("DHCP 0.28 (26 Sep 2026)\n");
    if (e || (argc && name[0]))
        return e ? e : info(&l);
    /* The broadcast interfaces, which are the Ethernets and the ones DHCP
     * would control. They are not the loopback and not a tunnel. */
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0)
        return NULL;
    char seen[16][16];
    int nseen = 0;
    for (struct ifaddrs *a = all; a && !e; a = a->ifa_next) {
        if ((a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_BROADCAST))
            continue;
        int dup = 0;
        for (int i = 0; i < nseen; i++)
            dup |= !strcmp(seen[i], a->ifa_name);
        if (dup || nseen == 16)
            continue;
        snprintf(seen[nseen++], 16, "%s", a->ifa_name);
        if (!read_lease(a->ifa_name, &l))
            e = info(&l);
    }
    freeifaddrs(all);
    return e;
}

/* *DHCPExecute [-e] [-b] [-p] [-w] <interface>: OS_ReadArgs'
 * "e/s,b=block/s,w/s,p/s,/a", by hand. */
static os_error *cmd_dhcpexecute(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    unsigned flags = 0;
    char name[16] = "";
    const char *t = ros_ptr(tail);
    while (*t >= ' ') {
        while (*t == ' ')
            t++;
        if (*t < ' ')
            break;
        char word[32];
        size_t n = 0;
        while (*t > ' ' && n < sizeof word - 1)
            word[n++] = *t++;
        word[n] = 0;
        while (*t > ' ')
            t++;
        if (!strcasecmp(word, "-e"))
            flags |= DECF_ERROR;
        else if (!strcasecmp(word, "-b") || !strcasecmp(word, "-block"))
            flags |= DECF_BLOCK;
        else if (!strcasecmp(word, "-w"))
            flags |= DECF_WAIT;
        else if (!strcasecmp(word, "-p"))
            flags |= DECF_PRIVATE;
        else if (word[0] != '-' && !name[0])
            snprintf(name, sizeof name, "%s", word);
        else
            return ros_error(0xDC, "Syntax: *DHCPExecute [-e] [-b] [-p] [-w] <interface>");
    }
    if (!name[0])
        return ros_error(0xDC, "Syntax: *DHCPExecute [-e] [-b] [-p] [-w] <interface>");

    os_error *e = NULL;
    for (;;) {
        uint32_t status;
        e = xdhcp_execute((flags & DECF_BLOCK ? DSE_BLOCK : 0) | (flags & DECF_PRIVATE ? DSE_PRIVATE : 0) | 1,
                          name, &status);
        if (e) {
            if (!(flags & DECF_WAIT) || e->errnum != ERROR_BASE + E_NO_SUCH_INTERFACE)
                break;
            e = NULL;
        } else {
            flags &= ~DECF_WAIT;
            if (status == S_BOUND || status == S_ABANDON)
                break;
        }
        if (inet_escape()) {
            e = ros_error(0x11, "Escape");
            break;
        }
        if (!(flags & (DECF_BLOCK | DECF_WAIT)))
            break;
        os_error *slept;                /* asleep: a task window's desktop goes on */
        if (ros_sleep_fd(-1, 0, 100, &slept) < 0) {
            e = slept;
            break;
        }
    }
    if (e && (flags & DECF_ERROR)) {
        set_var("Inet$Error", e->errmess);
        return NULL;
    }
    return e;
}

static const struct ros_command commands[] = {
    { "DHCPInfo", ROS_CMD_INFO(0, 1, 1, 0), "Syntax: *DHCPInfo [<interface>]",
      "*DHCPInfo displays the internal state of the DHCP module.", cmd_dhcpinfo },
    { "DHCPExecute", ROS_CMD_INFO(1, 5, 255, 0), "Syntax: *DHCPExecute [-e] [-b] [-p] [-w] <interface>",
      "*DHCPExecute instructs the DHCP module to control the configuration of the specified "
      "interface.\r\t-e  places any error message into Inet$Error\r\t-b  blocks until the "
      "interface is bound\r\t-w  waits for <interface> to appear\r\t-p  assigns a private IP "
      "address if DHCP request times out", cmd_dhcpexecute },
    { 0 },
};

/* ---- the module ----------------------------------------------------------------- */

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module dhcp_module = {
    .title = "DHCP",
    .help = "DHCP\t0.28 (26 Sep 2026) ROSGD native, over Linux's kernel DHCP",
    .bad_swi = bad_swi,
    .commands = commands,
    .swi_chunk = 0x52E80,
    .swi_thunks = ros_swi_thunks_DHCP,
    .swi_names = ros_swi_names_DHCP,
    .swi_prefix = "DHCP",
};

__attribute__((constructor)) static void count(void)
{
    dhcp_module.swi_count = ros_swi_count_DHCP;
}
