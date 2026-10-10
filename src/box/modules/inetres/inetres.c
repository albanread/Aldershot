/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* inetres.c -- the !Internet commands, as a native module over Linux.
 *
 * RISC OS 5 keeps its network tools in !Internet.bin: FreeBSD's ping,
 * traceroute, arp, ifconfig, route, netstat (InetStat) and sysctl, ported
 * to the Internet module's socket SWIs, and a few of its own: IPVars,
 * GetHost, IfRConfig and ShowStat (Sources/SystemRes/InetRes). ROSGD runs
 * no ARM programs, and its network stack is Linux's. So they are commands
 * of this module, each answering from Linux: its sockets, its /proc/net,
 * its ioctls. The commands keep the tools' syntax and options. Their
 * output is as FreeBSD's prints it, their errors are as err() prints them
 * ("ping: ..."), their exit codes are in Sys$ReturnCode, and they take
 * RISC OS's -e. An option they do not carry is an error and is never
 * guessed at.
 *
 * *SSH and *SSH-KeyGen are OpenSSH's ssh and ssh-keygen, run on the PTY
 * module's terminal (modules/pty). The command's tail is their arguments.
 *
 * The hosted build (macOS) has what is portable, which is ICMP sockets and
 * the interface list, and says so where a command needs Linux.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "inetres.h"
#include "pty.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#ifdef __linux__
#include <ifaddrs.h>
#include <net/if.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

/* ---- arguments ------------------------------------------------------------------ */

static void split(const char *cmd, uint32_t tail, struct inet_args *a)
{
    const char *t = ros_ptr(tail);
    size_t n = 0;
    a->argc = 0;
    a->argv[a->argc++] = strcpy(a->buf, cmd);
    n = strlen(cmd) + 1;
    while (*t >= ' ' && a->argc < INET_MAXARGS && n < sizeof a->buf - 1) {
        while (*t == ' ')
            t++;
        if (*t < ' ')
            break;
        a->argv[a->argc++] = a->buf + n;
        int quoted = *t == '"';
        if (quoted)
            t++;
        while (*t >= ' ' && (quoted ? *t != '"' : *t != ' ') && n < sizeof a->buf - 1)
            a->buf[n++] = *t++;
        if (quoted && *t == '"')
            t++;
        a->buf[n++] = 0;
    }
}

int inet_getopt(struct inet_opt *o, const struct inet_args *a, const char *spec)
{
    if (!o->ind)
        o->ind = 1;
    if (!o->next || !*o->next) {
        if (o->ind >= a->argc)
            return -1;
        const char *w = a->argv[o->ind];
        if (w[0] != '-' || !w[1])
            return -1;
        if (!strcmp(w, "--")) {
            o->ind++;
            return -1;
        }
        o->next = w + 1;
        o->ind++;
    }
    int c = (unsigned char)*o->next++;
    const char *p = strchr(spec, c);
    if (!p || c == ':')
        return '?';
    if (p[1] == ':') {
        if (*o->next) {
            o->arg = o->next;
            o->next = NULL;
        } else if (o->ind < a->argc) {
            o->arg = a->argv[o->ind++];
        } else {
            return ':';
        }
    }
    return c;
}

/* ---- output --------------------------------------------------------------------- */

os_error *inet_printf(const char *fmt, ...)
{
    char text[1024];
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

/* ---- the program's end ---------------------------------------------------------- */

void inet_set_var(const char *name, const char *value)
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

void inet_exit(int code)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    char *b = ros_rma_alloc(32);
    if (!b)
        return;
    strcpy(b, "Sys$ReturnCode");
    int32_t n = code;
    memcpy(b + 16, &n, 4);                          /* a number is a word */
    s.r[0] = ros_addr(b), s.r[1] = ros_addr(b + 16), s.r[2] = 4, s.r[3] = 0, s.r[4] = 1;
    ros_swi(&s, ROS_X_BIT | 0x24);                  /* OS_SetVarVal, a number */
    ros_rma_free(b);
}

os_error *inet_fail(const struct inet_tool *t, int code, const char *fmt, ...)
{
    char text[400];
    int n = snprintf(text, sizeof text, "%s: ", t->name);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text + n, sizeof text - (size_t)n, fmt, ap);
    va_end(ap);
    inet_exit(code);
    if (t->e_flag) {
        inet_set_var("Inet$Error", text);
        return NULL;
    }
    return inet_printf("%s\n", text);
}

int inet_escape(void)
{
    int esc = 0;
    xos_read_escape_state(&esc);
    if (esc) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 126;                               /* OS_Byte 126: acknowledge */
        ros_swi(&s, ROS_X_BIT | 0x06);
    }
    return esc;
}

/* ---- interface names ------------------------------------------------------------ */

void inet_riscos_ifname(const char *linux_name, char out[16])
{
    snprintf(out, 16, "%s", strcmp(linux_name, "lo") ? linux_name : "lo0");
}

int inet_linux_ifname(const char *riscos, char out[16])
{
    if (!strcmp(riscos, "lo0") || !strcmp(riscos, "lo")) {
#ifdef __linux__
        strcpy(out, "lo");
#else
        strcpy(out, "lo0");
#endif
        return 0;
    }
    if (if_nametoindex(riscos)) {
        snprintf(out, 16, "%s", riscos);
        return 0;
    }
    size_t l = strlen(riscos);
    if (!l || !isdigit((unsigned char)riscos[l - 1]))
        return -1;
    int want = riscos[l - 1] - '0', n = 0;
    struct ifaddrs *all, *a;
    if (getifaddrs(&all) < 0)
        return -1;
    char seen[8][16];
    int nseen = 0;
    for (a = all; a; a = a->ifa_next) {
        if (a->ifa_flags & IFF_LOOPBACK)
            continue;
        int dup = 0;
        for (int i = 0; i < nseen; i++)
            dup |= !strcmp(seen[i], a->ifa_name);
        if (dup || nseen == 8)
            continue;
        snprintf(seen[nseen++], 16, "%s", a->ifa_name);
        if (n++ == want) {
            snprintf(out, 16, "%s", a->ifa_name);
            freeifaddrs(all);
            return 0;
        }
    }
    freeifaddrs(all);
    return -1;
}

/* ---- the module ------------------------------------------------------------------ */

#define RUN(fn, name)                                                   \
    static os_error *cmd_##fn(struct ros_module *m, uint32_t tail, uint32_t argc) \
    {                                                                   \
        (void)m, (void)argc;                                            \
        struct inet_args a;                                             \
        split(name, tail, &a);                                          \
        inet_exit(0);                                                   \
        return inet_##fn(&a);                                           \
    }

RUN(ping, "ping")
RUN(traceroute, "traceroute")
RUN(arp, "arp")
RUN(ifconfig, "ifconfig")
RUN(route, "route")
RUN(inetstat, "inetstat")
RUN(sysctl, "sysctl")
RUN(ipvars, "ipvars")
RUN(gethost, "gethost")
RUN(ifrconfig, "ifrconfig")
RUN(showstat, "showstat")
RUN(tftp, "tftp")
RUN(checkmem, "checkmem")
RUN(readcmosip, "readcmosip")
RUN(triggercbs, "triggercbs")

/* *MD5 and its family: one command, the digest named by argv[0] */
#define DIGEST(id, name)                                                \
    static os_error *cmd_##id(struct ros_module *m, uint32_t tail, uint32_t argc) \
    {                                                                   \
        (void)m, (void)argc;                                            \
        struct inet_args a;                                             \
        split(name, tail, &a);                                          \
        inet_exit(0);                                                   \
        return inet_md5(&a);                                            \
    }

DIGEST(md5, "md5")
DIGEST(sha1, "sha1")
DIGEST(sha224, "sha224")
DIGEST(sha256, "sha256")
DIGEST(sha384, "sha384")
DIGEST(sha512, "sha512")
DIGEST(sha512t256, "sha512t256")
DIGEST(rmd160, "rmd160")
DIGEST(skein256, "skein256")
DIGEST(skein512, "skein512")
DIGEST(skein1024, "skein1024")
DIGEST(md5sum, "md5sum")
DIGEST(sha1sum, "sha1sum")
DIGEST(sha224sum, "sha224sum")
DIGEST(sha256sum, "sha256sum")
DIGEST(sha384sum, "sha384sum")
DIGEST(sha512sum, "sha512sum")
DIGEST(sha512t256sum, "sha512t256sum")
DIGEST(rmd160sum, "rmd160sum")
DIGEST(skein256sum, "skein256sum")
DIGEST(skein512sum, "skein512sum")
DIGEST(skein1024sum, "skein1024sum")

/* *SSH and *SSH-KeyGen: OpenSSH's programs on the PTY module's terminal.
 * The command's tail is their arguments as it stands. */
static os_error *terminal(const char *program, uint32_t tail)
{
    char line[1024];
    const char *t = ros_ptr(tail);
    size_t n = (size_t)snprintf(line, sizeof line, "%s ", program);
    while ((uint8_t)*t >= ' ' && n < sizeof line - 1)
        line[n++] = *t++;
    line[n] = 0;
    int status = 0;
    inet_exit(0);
    os_error *e = pty_terminal(line, &status);
    if (!e)
        inet_exit(status);
    return e;
}

static os_error *cmd_ssh(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return terminal("ssh", tail);
}

static os_error *cmd_sshkeygen(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return terminal("ssh-keygen", tail);
}

#define C(n, syntax, help, fn) \
    { n, ROS_CMD_INFO(0, 255, 0, 0), "Syntax: *" syntax, help, cmd_##fn }

#define D(n, fn, alg)                                                                             \
    C(n, n " [-qrtx] [-c <expected>] [-s <string>] [<files> ...]",                                \
      "*" n " prints the " alg " digest of each file, or of -s's string: \"" alg " (file) = <hex>\".", fn)
#define S(n, fn, alg)                                                                             \
    C(n, n " [-bqrx] [-c <file>] [-s <string>] [<files> ...]",                                     \
      "*" n " prints the " alg " digest of each file as GNU's coreutils do, or checks the digests " \
      "listed in -c's file.", fn)

static const struct ros_command commands[] = {
    C("ARP", "ARP [-n] [-i <interface>] -a | <host>, -d <host> | -a, -s <host> <ether> [temp] [pub]",
      "*ARP shows and changes the table of Ethernet addresses: Linux's neighbour table.", arp),
    C("CheckMem", "CheckMem",
      "*CheckMem made sure the configured RMA and system heap were big enough for the Internet stack; "
      "ROSGD's are the arena's, so it has nothing to do.", checkmem),
    C("GetHost", "GetHost <host>", "*GetHost looks a host up by name or address.", gethost),
    C("IfConfig", "IfConfig [-e] [-a | -d | -u | -l] [<interface> [<address>] [netmask <mask>] "
                  "[broadcast <addr>] [mtu <n>] [up | down]]",
      "*IfConfig shows and sets the network interfaces' addresses and flags.", ifconfig),
    C("IfRConfig", "IfRConfig", "*IfRConfig reconfigures the interfaces from the DHCP lease.", ifrconfig),
    C("InetStat", "InetStat [-a] [-n] [-r] [-i] [-s] [-p <protocol>]",
      "*InetStat shows the network's status: sockets, routes, interfaces, statistics.", inetstat),
    C("IPVars", "IPVars [-e] [<interface>]",
      "*IPVars sets Inet$ variables from an interface's address.", ipvars),
    C("Ping", "Ping [-nq] [-c <count>] [-i <wait>] [-m <ttl>] [-s <size>] [-t <timeout>] [-W <waittime>] <host>",
      "*Ping sends ICMP echo requests to a host until Escape (or -c's count), and counts the replies.", ping),
    C("Route", "Route [-e] [-n] add|delete|get|show|flush [-net|-host] <dest> [<gateway>] [netmask <mask>]",
      "*Route shows and changes the routing table.", route),
    C("ReadCMOSIP", "ReadCMOSIP", "*ReadCMOSIP sets Inet$CMOSIPAddr from the IP address kept in CMOS.", readcmosip),
    C("ShowStat", "ShowStat", "*ShowStat shows the network statistics.", showstat),
    C("SSH", "SSH [<options>] [<user>@]<host> [<command>]",
      "*SSH logs in to a host, or runs a command there: OpenSSH's ssh, on a terminal the size of the "
      "text window. Its keys and known hosts are in the home directory's .ssh: HostFS::Host.$./ssh "
      "on the box.", ssh),
    C("SSH-KeyGen", "SSH-KeyGen [<options>]",
      "*SSH-KeyGen makes, changes and lists SSH keys: OpenSSH's ssh-keygen.", sshkeygen),
    C("SysCtl", "SysCtl [-e] [-n] [-a] <variable>[=<value>] ...",
      "*SysCtl reads and sets the network's kernel variables.", sysctl),
    C("Tftp", "Tftp [<host> [<port>]] | tftp://<host>/<file>[;mode=<mode>]",
      "*Tftp transfers files to and from a TFTP server: a tftp> prompt of commands, or a URI fetched at once.",
      tftp),
    D("MD5", md5, "MD5"),
    D("SHA1", sha1, "SHA-1"),
    D("SHA224", sha224, "SHA-224"),
    D("SHA256", sha256, "SHA-256"),
    D("SHA384", sha384, "SHA-384"),
    D("SHA512", sha512, "SHA-512"),
    D("SHA512t256", sha512t256, "SHA-512/256"),
    D("RMD160", rmd160, "RIPEMD-160"),
    D("Skein256", skein256, "Skein-256"),
    D("Skein512", skein512, "Skein-512"),
    D("Skein1024", skein1024, "Skein-1024"),
    S("MD5Sum", md5sum, "MD5"),
    S("SHA1Sum", sha1sum, "SHA-1"),
    S("SHA224Sum", sha224sum, "SHA-224"),
    S("SHA256Sum", sha256sum, "SHA-256"),
    S("SHA384Sum", sha384sum, "SHA-384"),
    S("SHA512Sum", sha512sum, "SHA-512"),
    S("SHA512t256Sum", sha512t256sum, "SHA-512/256"),
    S("RMD160Sum", rmd160sum, "RIPEMD-160"),
    S("Skein256Sum", skein256sum, "Skein-256"),
    S("Skein512Sum", skein512sum, "Skein-512"),
    S("Skein1024Sum", skein1024sum, "Skein-1024"),
    C("TriggerCBs", "TriggerCBs", "*TriggerCBs lets pending callbacks run.", triggercbs),
    C("TraceRoute", "TraceRoute [-nI] [-f <first_ttl>] [-m <max_ttl>] [-q <nqueries>] [-w <waittime>] <host> [<packetlen>]",
      "*TraceRoute prints the route packets take to a host.", traceroute),
    { 0 },
};

struct ros_module inetres_module = {
    .title = "InetRes",
    .help = "InetRes\t1.00 (26 Sep 2026) ROSGD native: the !Internet commands, over Linux",
    .commands = commands,
};
