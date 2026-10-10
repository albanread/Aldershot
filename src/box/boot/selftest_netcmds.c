/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_netcmds.c: the network's commands, as shims over Linux: InetRes
 * (the !Internet tools), *InetInfo and *InetGateway (Internet),
 * *ResolverConfig (Resolver), and DHCP's commands and SWIs.
 *
 * Over loopback everywhere: every command is there. Ping, IfConfig and
 * GetHost answer about 127.0.0.1, errors go to Inet$Error with -e, and DHCP's
 * SWIs read a lease and edit a packet. The digests (*MD5, *SHA256 and the
 * rest) pass FreeBSD's test suite and read files. *Tftp fetches and sends
 * files, with options, from a TFTP server of the test's own on loopback.
 * In the box, which is Linux, the tools that read /proc/net work.
 * TraceRoute and InetStat run over loopback too. With a network (ip=dhcp),
 * *DHCPExecute sets the Inet$ variables from the kernel's lease, Route and
 * ARP see the gateway, and *ResolverConfig writes /etc/resolv.conf and links
 * it back.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "fileswitch.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check
#define WRCHV 0x03u

static char out[4096];
static unsigned outn;
static char last_error[256];
static uint32_t line;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI: the error number or 0; the output in out, as the VDU stream had it */
static uint32_t cli(const char *cmd)
{
    snprintf(ros_ptr(line), 256, "%s", cmd);
    outn = 0, out[0] = 0;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line;
    ros_swi(&s, XOS_CLI);
    snprintf(last_error, sizeof last_error, "%s", s.v ? ((os_error *)ros_ptr(s.r[0]))->errmess : "");
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static int says_part(const char *cmd, const char *part)
{
    uint32_t e = cli(cmd);
    int ok = e == 0 && strstr(out, part) != NULL;
    if (!ok)
        ros_console_printf("        %s: &%X %s \"%s\"\n", cmd, e, last_error, out);
    return ok;
}

static const char *var(const char *name)
{
    static char value[256];
    char *b = ros_rma_alloc(512);
    value[0] = 0;
    if (!b)
        return value;
    snprintf(b, 256, "%s", name);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(b), s.r[1] = ros_addr(b + 256), s.r[2] = 255, s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, ROS_X_BIT | 0x23);                  /* OS_ReadVarVal, expanded */
    if (!s.v)
        snprintf(value, sizeof value, "%.*s", (int)s.r[2], b + 256);
    ros_rma_free(b);
    return value;
}

/* The Sys$ReturnCode a tool left */
static int rc(void)
{
    int n = -1;
    sscanf(var("Sys$ReturnCode"), "%d", &n);
    return n;
}

static void commands_known(void)
{
    static const char *const names[] = { "ARP", "GetHost", "IfConfig", "IfRConfig", "InetStat", "IPVars",
                                         "Ping", "Route", "ShowStat", "SysCtl", "TraceRoute", "InetInfo",
                                         "InetGateway", "ResolverConfig", "DHCPInfo", "DHCPExecute", "Tftp",
                                         "MD5", "SHA256", "Skein1024", "SHA512Sum", "RMD160Sum", "CheckMem",
                                         "ReadCMOSIP", "TriggerCBs" };
    int ok = 1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char cmd[64], want[64];
        snprintf(cmd, sizeof cmd, "Help %s", names[i]);
        snprintf(want, sizeof want, "Syntax: *%s", names[i]);
        ok &= says_part(cmd, want);
    }
    check(ok, "Network commands: the !Internet tools, *InetInfo, *InetGateway, *ResolverConfig, DHCP's two",
          "a command is missing");
    check(cli("CheckMem") == 0 && !out[0] && cli("TriggerCBs") == 0 && !out[0],
          "*CheckMem and *TriggerCBs -- !Internet's Startup utilities, quietly", "%s \"%s\"", last_error, out);
}

static void over_loopback(void)
{
    int ping = says_part("Ping -c 1 127.0.0.1", "1 packets transmitted, 1 packets received, 0.0% packet loss") &&
               strstr(out, "PING 127.0.0.1 (127.0.0.1): 56 data bytes") && strstr(out, "icmp_seq=0") &&
               rc() == 0;
    check(ping, "*Ping -c 1 127.0.0.1 -- FreeBSD's lines, Sys$ReturnCode 0", "\"%s\"", out);

    int unknown = cli("Ping -c 1 nosuch.invalid") == 0 &&
                  !strcmp(out, "ping: cannot resolve nosuch.invalid: Unknown host\n\r") && rc() == 68;
    check(unknown, "*Ping of an unknown host -- err()'s line, Sys$ReturnCode 68", "\"%s\" rc %d", out, rc());

    int ifc = says_part("IfConfig lo0", "lo0: flags=") && strstr(out, "<UP,LOOPBACK,RUNNING") &&
              strstr(out, "inet 127.0.0.1 netmask 0xff000000");
    check(ifc, "*IfConfig lo0 -- flags and address as FreeBSD's ifconfig prints them", "\"%s\"", out);

    int e = cli("IfConfig -e nosuch") == 0 && !out[0] && strstr(var("Inet$Error"), "IfConfig: ");
    check(e, "*IfConfig -e -- the error in Inet$Error, nothing printed", "\"%s\" Inet$Error \"%s\"", out,
          var("Inet$Error"));

    int host = says_part("GetHost localhost", "Hostname: localhost") && strstr(out, "Address:  127.0.0.1");
    check(host, "*GetHost localhost", "\"%s\"", out);

    int vars = cli("IPVars lo0") == 0 && !strcmp(var("Inet$lo0$Addr"), "127.0.0.1") &&
               !strcmp(var("Inet$lo0$Mask"), "255.0.0.0");
    check(vars, "*IPVars lo0 -- Inet$lo0$Addr and $Mask", "Addr \"%s\" Mask \"%s\"", var("Inet$lo0$Addr"),
          var("Inet$lo0$Mask"));

    int info = says_part("InetInfo", "Resource Usage:\n\r\n\rSockets active 0\n\r\n\rPacket forwarding ") &&
               says_part("InetInfo i", "Name  MTU    Flags     (ifp)\n\rlo0   ");
    check(info, "*InetInfo -- resource usage, interfaces, forwarding", "\"%s\"", out);

    int gw = says_part("InetGateway", "Packet forwarding ") && cli("InetGateway sideways") == 0xDC;
    check(gw, "*InetGateway -- the state, and its syntax", "\"%s\" %s", out, last_error);
}

static void dhcp(void)
{
    uint32_t v = 0, status = 99;
    int32_t left = 0;
    check(!xdhcp_version(&v) && v == 28, "DHCP_Version -- 0.28", "%u", v);

    uint8_t *pkt = ros_rma_alloc(600);
    uint32_t p = ros_addr(pkt);
    os_error *err = xdhcp_get_state(0, "lo0", p, 600, &status, &left);
    int state = !err && status == 3 && left == 600 - 548 && pkt[0] == 2 && !memcmp(pkt + 16, "\x7f\0\0\1", 4) &&
                !memcmp(pkt + 236, "\x63\x82\x53\x63", 4);
    check(state, "DHCP_GetState lo0 -- BOUND, a DHCPACK for 127.0.0.1", "%s status %u left %d",
          err ? err->errmess : "", status, left);

    uint8_t *opt = ros_rma_alloc(16);
    char *name = ros_rma_alloc(8);
    strcpy(name, "lo0");
    err = xdhcp_get_option(0, ros_addr(name), 1, ros_addr(opt), 16, &status, &left);
    int mask = !err && left == 12 && !memcmp(opt, "\xff\0\0\0", 4);
    check(mask, "DHCP_GetOption lo0, 1 -- the netmask", "%s left %d", err ? err->errmess : "", left);

    memcpy(opt, "rosgd", 5);
    os_error *e1 = xdhcp_set_option(1, p, 12, ros_addr(opt), 5, &status);
    memset(opt, 0, 16);
    os_error *e2 = xdhcp_get_option(1, p, 12, ros_addr(opt), 16, &status, &left);
    os_error *e3 = xdhcp_set_option(1, p, 12, 0, 0, &status);
    os_error *e4 = xdhcp_get_option(1, p, 12, ros_addr(opt), 16, &status, &left);
    int edit = !e1 && !e2 && !memcmp(opt, "rosgd", 5) && !e3 && e4 && e4->errnum == 0x816C02;
    check(edit, "DHCP_SetOption on a packet -- added, read back, deleted", "%s", e4 ? e4->errmess : "still there");

    err = xdhcp_set_option(0, ros_addr(name), 12, ros_addr(opt), 5, &status);
    check(err && err->errnum == 0x816C03, "DHCP_SetOption on an interface -- the kernel's request, not added",
          "%s", err ? err->errmess : "added");
    err = xdhcp_get_option(0, ros_addr(name), 255, 0, 0, &status, &left);
    check(err && err->errnum == 0x816C01, "DHCP_GetOption 255 -- an illegal option", "%s",
          err ? err->errmess : "accepted");

    int ex = cli("DHCPExecute nosuch") == 0x816C00 && cli("DHCPExecute -e nosuch") == 0 &&
             !strcmp(var("Inet$Error"), "No such interface under DHCP control");
    check(ex, "*DHCPExecute of no interface -- the error, or with -e Inet$Error", "%s", last_error);

    int info = says_part("DHCPInfo lo0", "DHCP 0.28 (26 Sep 2026)\n\r\n\rInterface     : lo0\n\r"
                                         "State         : BOUND\n\rIP address    : 127.0.0.1\n\r");
    check(info, "*DHCPInfo lo0 -- the original's layout", "\"%s\"", out);
    ros_rma_free(pkt), ros_rma_free(opt), ros_rma_free(name);

    check(cli("ResolverConfig") == 0 && !xresolver_cache_control(3),
          "*ResolverConfig and Resolver_CacheControl 3 -- the configuration re-read", "%s", last_error);
}

/* ---- the digests, and *Tftp, on a disc of the test's own ---------------- */

static char root[512];

static void put_host(const char *name, const char *data, size_t n)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s", root, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

/* A file on the disc, as HostFS keeps it (",ffd" for Data), compared */
static int host_is(const char *name, const char *want, size_t n)
{
    char path[600], got[4096];
    snprintf(path, sizeof path, "%s/%s", root, name);
    FILE *f = fopen(path, "rb");
    size_t len = f ? fread(got, 1, sizeof got, f) : 0;
    if (f)
        fclose(f);
    return f && len == n && !memcmp(got, want, n);
}

static void digests(void)
{
    static const char *const names[] = { "MD5", "SHA1", "SHA224", "SHA256", "SHA384", "SHA512",
                                         "SHA512t256", "RMD160", "Skein256", "Skein512", "Skein1024" };
    int ok = 1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char cmd[40];
        snprintf(cmd, sizeof cmd, "%s -x", names[i]);
        int good = cli(cmd) == 0 && !strstr(out, "INCORRECT") && rc() == 0;
        int n = 0;
        for (const char *p = out; (p = strstr(p, "verified correct")); p++)
            n++;
        if (!good || n != 8)
            ros_console_printf("        %s: \"%.200s\"\n", cmd, out);
        ok &= good && n == 8;
    }
    check(ok, "*MD5, *SHA1 ... *Skein1024 -x -- FreeBSD's test suite, every digest verified", "a digest failed");

    int s = cli("SHA256 -s abc") == 0 &&
            !strcmp(out, "SHA256 (abc) = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n\r") &&
            cli("SHA512t256 -q -s abc") == 0 &&
            !strcmp(out, "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23\n\r");
    check(s, "*SHA256 -s, *SHA512t256 -q -s -- a string's digest", "\"%s\"", out);

    put_host("abc", "abc", 3);
    static const char sums[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  HostFS::Net.$.abc\n"
                               "0000000000000000000000000000000000000000000000000000000000000000  HostFS::Net.$.abc\n";
    put_host("sums", sums, sizeof sums - 1);
    int f = cli("SHA256 HostFS::Net.$.abc") == 0 &&
            !strcmp(out, "SHA256 (HostFS::Net.$.abc) = "
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n\r") &&
            cli("MD5Sum HostFS::Net.$.abc") == 0 &&
            !strcmp(out, "900150983cd24fb0d6963f7d28e17f72  HostFS::Net.$.abc\n\r");
    check(f, "*SHA256 and *MD5Sum of a file -- FreeBSD's line, and GNU's", "\"%s\"", out);

    int c = cli("SHA256Sum -c HostFS::Net.$.sums") == 0 &&
            !strcmp(out, "HostFS::Net.$.abc: OK\n\rHostFS::Net.$.abc: FAILED\n\r"
                         "sha256sum: WARNING: 1 computed checksums did NOT match\n\r") &&
            rc() == 2 && cli("SHA256 HostFS::Net.$.Nothing") == 0 &&
            !strcmp(out, "sha256: unable to open HostFS::Net.$.Nothing for reading\n\r") && rc() == 1;
    check(c, "*SHA256Sum -c, and a file not there -- Sys$ReturnCode 2 and 1", "\"%s\" rc %d", out, rc());
}

/* A TFTP server on loopback, of three requests: RRQ answered with an OACK if
 * it asks blksize, then the file in blocks; WRQ taken in; and a second WRQ
 * whose first DATA is answered with a stray DATA packet before the ACK. */
static struct {
    int s;
    uint16_t port;
    uint8_t got[4096];
    size_t ngot;
    char options[64];               /* the RRQ's, as "name=value " */
    int stray_sent, data1;          /* the third request: a stray packet, and DATA 1 seen */
} srv;

static const char served[] = "line one\nline two\n" "0123456789012345678901234567890123456789";

static ssize_t wait_recv(int s, uint8_t *buf, size_t n, struct sockaddr_in *from)
{
    struct pollfd p = { .fd = s, .events = POLLIN };
    if (poll(&p, 1, 5000) <= 0)
        return -1;
    socklen_t fl = sizeof *from;
    return recvfrom(s, buf, n, 0, (struct sockaddr *)from, &fl);
}

static void *server(void *arg)
{
    (void)arg;
    uint8_t buf[2048];
    for (int req = 0; req < 3; req++) {
        struct sockaddr_in from;
        ssize_t n = wait_recv(srv.s, buf, sizeof buf, &from);
        if (n < 4)
            return NULL;
        int t = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        bind(t, (struct sockaddr *)&me, sizeof me);
        int op = buf[1];
        const char *p = (const char *)buf + 2, *end = (const char *)buf + n;
        p += strlen(p) + 1;                         /* the name */
        p += strlen(p) + 1;                         /* the mode */
        int blk = 512;
        if (op == 1)
            srv.options[0] = 0;
        while (p < end && *p) {
            const char *v = p + strlen(p) + 1;
            if (op == 1)
                snprintf(srv.options + strlen(srv.options), sizeof srv.options - strlen(srv.options), "%s=%s ", p,
                         v);
            if (!strcmp(p, "blksize"))
                blk = atoi(v);
            p = v + strlen(v) + 1;
        }
        uint8_t pkt[2048], ack[64];
        if (op == 1) {
            if (blk != 512) {                       /* OACK blksize, and its ACK 0 */
                int l = 2 + sprintf((char *)pkt + 2, "blksize%c%d", 0, blk) + 1;
                pkt[0] = 0, pkt[1] = 6;
                sendto(t, pkt, (size_t)l, 0, (struct sockaddr *)&from, sizeof from);
                wait_recv(t, ack, sizeof ack, &from);
            }
            size_t len = sizeof served - 1, at = 0;
            for (int block = 1;; block++) {
                size_t take = len - at < (size_t)blk ? len - at : (size_t)blk;
                pkt[0] = 0, pkt[1] = 3, pkt[2] = (uint8_t)(block >> 8), pkt[3] = (uint8_t)block;
                memcpy(pkt + 4, served + at, take);
                sendto(t, pkt, take + 4, 0, (struct sockaddr *)&from, sizeof from);
                if (wait_recv(t, ack, sizeof ack, &from) < 4)
                    break;
                at += take;
                if (take < (size_t)blk)
                    break;
            }
        } else {
            if (blk != 512) {                       /* OACK blksize */
                int l = 2 + sprintf((char *)pkt + 2, "blksize%c%d", 0, blk) + 1;
                pkt[0] = 0, pkt[1] = 6;
                sendto(t, pkt, (size_t)l, 0, (struct sockaddr *)&from, sizeof from);
            } else {
                uint8_t a0[4] = { 0, 4, 0, 0 };
                sendto(t, a0, 4, 0, (struct sockaddr *)&from, sizeof from);
            }
            for (;;) {
                ssize_t m = wait_recv(t, pkt, sizeof pkt, &from);
                if (m < 4)
                    break;
                if (req == 2) {
                    srv.data1 += pkt[1] == 3 && pkt[3] == 1;
                    if (!srv.stray_sent) {              /* not an ACK, where an ACK should be */
                        uint8_t stray[4] = { 0, 3, 0, 9 };
                        srv.stray_sent = 1;
                        sendto(t, stray, 4, 0, (struct sockaddr *)&from, sizeof from);
                        continue;
                    }
                }
                if (req != 2 && srv.ngot + (size_t)(m - 4) <= sizeof srv.got)
                    memcpy(srv.got + srv.ngot, pkt + 4, (size_t)(m - 4)), srv.ngot += (size_t)(m - 4);
                uint8_t a[4] = { 0, 4, pkt[2], pkt[3] };
                sendto(t, a, 4, 0, (struct sockaddr *)&from, sizeof from);
                if (m - 4 < blk)
                    break;
            }
        }
        close(t);
    }
    return NULL;
}

static void tftp(void)
{
    srv.s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t ml = sizeof me;
    pthread_t th;
    if (srv.s < 0 || bind(srv.s, (struct sockaddr *)&me, sizeof me) < 0 ||
        getsockname(srv.s, (struct sockaddr *)&me, &ml) < 0 || pthread_create(&th, NULL, server, NULL)) {
        check(0, "*Tftp -- a server on loopback", "no server: %s", strerror(errno));
        return;
    }
    srv.port = ntohs(me.sin_port);
    char cmds[512];
    int n = snprintf(cmds, sizeof cmds,
                     "connect 127.0.0.1 %u\rblocksize 16\rget served HostFS::Net.$.Got\r"
                     "ascii\rput HostFS::Net.$.Got sent\rquit\r", srv.port);
    put_host("Cmds", cmds, (size_t)n);
    uint32_t e = cli("Tftp { < HostFS::Net.$.Cmds }");
    char first_out[sizeof out], first_error[sizeof last_error];
    memcpy(first_out, out, sizeof out);
    memcpy(first_error, last_error, sizeof last_error);
    int first_rc = rc();
    /* a stray packet where an ACK should be: the block is sent again */
    n = snprintf(cmds, sizeof cmds, "connect 127.0.0.1 %u\rput HostFS::Net.$.Got stray\rquit\r", srv.port);
    put_host("Cmds2", cmds, (size_t)n);
    uint32_t e3 = cli("Tftp { < HostFS::Net.$.Cmds2 }");
    ROS_BLOCKING(pthread_join(th, NULL));
    close(srv.s);
    char crlf[128];
    size_t k = 0;                                   /* the file, as netascii sends it */
    for (const char *p = served; *p; p++) {
        if (*p == '\n')
            crlf[k++] = '\r';
        crlf[k++] = *p;
    }
    int ok = !e && strstr(first_out, "Blocksize is now 16 bytes.") &&
             strstr(first_out, "Received 58 bytes during") && strstr(first_out, "Sent 60 bytes during") &&
             !strcmp(srv.options, "tsize=0 blksize=16 rollover=0 ") && host_is("Got,ffd", served, sizeof served - 1) &&
             srv.ngot == k && !memcmp(srv.got, crlf, k) && first_rc == 1;
    check(ok, "*Tftp -- get with blksize 16 and tsize, put in netascii; FreeBSD's lines",
          "&%X %s options \"%s\" sent %zu \"%.300s\"", e, first_error, srv.options, srv.ngot, first_out);
    check(!e3 && srv.stray_sent && srv.data1 == 2 && strstr(out, "Sent 58 bytes during"),
          "*Tftp -- a packet that is not an ACK makes the window be sent again",
          "&%X %s stray %d, DATA 1 seen %d times \"%.300s\"", e3, last_error, srv.stray_sent, srv.data1, out);
}

static void on_a_disc(void)
{
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-net-XXXXXX", tmp);
    if (!mkdtemp(root)) {
        check(0, "Network commands -- a directory for the test disc", "none");
        return;
    }
    ros_hostfs_mount("Net", root);
    digests();
    tftp();
    ros_hostfs_unmount("Net");
    static const char *const files[] = { "abc", "sums", "Cmds", "Cmds2", "Got,ffd", NULL };
    for (int i = 0; files[i]; i++) {
        char path[600];
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        remove(path);
    }
    rmdir(root);
}

#ifdef __linux__
/* The box's own: /proc/net, and the kernel's lease when there is a network */
static void in_the_box(void)
{
    int tr = says_part("TraceRoute -n -q 1 -w 1 127.0.0.1", "traceroute to 127.0.0.1 (127.0.0.1), 64 hops max") &&
             strstr(out, " 1  127.0.0.1  ");
    check(tr, "*TraceRoute over loopback -- one hop, the port unreachable ends it", "\"%s\"", out);
    check(says_part("TraceRoute -n 127.0.0.1 99999999", "packet size must be at most"),
          "*TraceRoute -- a packet size beyond the probe's buffer is refused", "\"%s\"", out);

    int st = says_part("InetStat -an", "Active Internet connections (including servers)") &&
             says_part("InetStat -i", "Name    Mtu Network");
    check(st, "*InetStat -an, -i -- sockets and interfaces from /proc/net", "\"%s\"", out);

    check(says_part("SysCtl net.inet.ip.ttl", "net.inet.ip.ttl: 64"), "*SysCtl net.inet.ip.ttl -- 64",
          "\"%s\"", out);

    if (!ros_cmdline_has("ip=dhcp"))
        return;                                     /* no network: the lease is not there to read */
    cli("Unset Inet$Error");
    check(cli("DHCPExecute -e eth0") == 0 && !var("Inet$Error")[0], "*DHCPExecute -e eth0 -- no error",
          "Inet$Error \"%s\"", var("Inet$Error"));
    const char *addr = var("Inet$EtherIPAddr");
    int lease = addr[0] && strcmp(addr, "0.0.0.0") && var("Inet$Gateway")[0] && var("Inet$EtherIPMask")[0];
    char gw[32];
    snprintf(gw, sizeof gw, "%s", var("Inet$Gateway"));
    check(lease, "*DHCPExecute eth0 -- the kernel's lease in Inet$EtherIPAddr, $EtherIPMask, $Gateway",
          "addr \"%s\" gateway \"%s\" resolvers \"%s\"", addr, gw, var("Inet$Resolvers"));

    char cmd[80];
    snprintf(cmd, sizeof cmd, "Route -n get %s", gw);
    int route = says_part("InetStat -rn", "default") && says_part(cmd, "interface: eth0");
    check(route, "*InetStat -r and *Route get -- the default route through eth0", "\"%s\"", out);
    snprintf(cmd, sizeof cmd, "Ping -c 1 -n %s", gw);
    cli(cmd);
    snprintf(cmd, sizeof cmd, "ARP -n %s", gw);
    int arp = says_part(cmd, gw) && strstr(out, " on eth0");
    check(arp, "*ARP -- the gateway's entry after a ping", "\"%s\"", out);

    check(says_part("DHCPInfo eth0", "State         : BOUND"), "*DHCPInfo eth0 -- BOUND", "\"%s\"", out);

    /* *ResolverConfig: Inet$Resolvers written to /etc/resolv.conf, and an
     * empty one links it back to the kernel's /proc/net/pnp */
    cli("Set Inet$Resolvers 192.0.2.53");
    cli("Set Inet$ResolverRetries 2");
    uint32_t e = cli("ResolverConfig");
    FILE *f = fopen("/etc/resolv.conf", "r");
    char text[256] = "";
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    text[n] = 0;
    if (f)
        fclose(f);
    cli("Unset Inet$ResolverRetries");
    cli("Set Inet$Resolvers \"\"");
    uint32_t e2 = cli("ResolverConfig");
    char link[64] = "";
    ssize_t ln = readlink("/etc/resolv.conf", link, sizeof link - 1);
    if (ln > 0)
        link[ln] = 0;
    int rcf = !e && strstr(text, "nameserver 192.0.2.53\n") && strstr(text, "options attempts:2\n") && !e2 &&
              !strcmp(link, "/proc/net/pnp");
    check(rcf, "*ResolverConfig -- /etc/resolv.conf from Inet$Resolvers, then back to the kernel's",
          "\"%s\" link \"%s\" %s", text, link, last_error);
}
#endif

void ros_selftest_netcmds(void)
{
    char *b = ros_rma_alloc(256);
    line = ros_addr(b);
    ros_vector_claim_native(WRCHV, wrch, 0);
    commands_known();
    over_loopback();
    dhcp();
    on_a_disc();
#ifdef __linux__
    if (getpid() == 1)
        in_the_box();
#endif
    ros_vector_release_native(WRCHV, wrch, 0);
    ros_rma_free(b);
}
