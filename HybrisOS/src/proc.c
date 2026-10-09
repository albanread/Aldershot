/* proc.c: the HAL's /proc files.  These provide what the box reads from
 * /proc under Linux.  The HAL makes each file's text from its own state when
 * the box opens the file.
 *
 *   /proc/cmdline, /proc/meminfo         (these are in syscall.c)
 *   /proc/net/pnp                        the name servers given by DHCP
 *   /proc/net/tcp, udp, route, dev, arp, snmp
 *                                        lwIP's sockets, routes and ARP
 *                                        table, in Linux's layouts, for
 *                                        *InetStat, *Route and *Arp
 *   /proc/sys/kernel/hostname, /proc/sys/net/ipv4/...
 *                                        the values *SysCtl shows; these are
 *                                        lwIP's, fixed when it is built
 *
 * All of the files are read-only. */
#include "hal.h"

#include "lwip/netif.h"
#include "lwip/etharp.h"

struct netif *net_iface(int i);
int sock_proc_net(char *out, size_t max, int udp);

static char text[16384];

static const struct { const char *path, *value; } sysctls[] = {
    { "/proc/sys/kernel/hostname", "rosgd\n" },
    { "/proc/sys/net/ipv4/ip_forward", "0\n" },
    { "/proc/sys/net/ipv4/ip_default_ttl", "64\n" },
    { "/proc/sys/net/ipv4/ip_local_port_range", "49152\t65535\n" },     /* lwIP's range */
    { "/proc/sys/net/ipv4/icmp_echo_ignore_broadcasts", "0\n" },
    { "/proc/sys/net/ipv4/tcp_wmem", "4096\t58400\t58400\n" },           /* TCP_SND_BUF */
    { "/proc/sys/net/ipv4/tcp_rmem", "4096\t58400\t58400\n" },           /* TCP_WND */
    { "/proc/sys/net/ipv4/tcp_keepalive_time", "7200\n" },
    { "/proc/sys/net/ipv4/tcp_keepalive_intvl", "75\n" },
    { "/proc/sys/net/ipv4/tcp_timestamps", "0\n" },
    { "/proc/sys/net/ipv4/tcp_syncookies", "0\n" },
};

static const char *if_name(int k) { return k ? "eth0" : "lo"; }

static const char *route(void)
{
    char *p = text, *end = text + sizeof text;
    p += ksnprintf(p, (size_t)(end - p),
                   "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    struct netif *n = net_iface(1);
    if (n && ip4_addr_get_u32(netif_ip4_addr(n))) {
        uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(n)), mask = ip4_addr_get_u32(netif_ip4_netmask(n)),
                 gw = ip4_addr_get_u32(netif_ip4_gw(n));
        if (gw)
            p += ksnprintf(p, (size_t)(end - p), "eth0\t00000000\t%08X\t0003\t0\t0\t0\t00000000\t0\t0\t0\n", gw);
        ksnprintf(p, (size_t)(end - p), "eth0\t%08X\t00000000\t0001\t0\t0\t0\t%08X\t0\t0\t0\n", ip & mask, mask);
    }
    return text;
}

static const char *dev(void)
{
    char *p = text, *end = text + sizeof text;
    p += ksnprintf(p, (size_t)(end - p),
                   "Inter-|   Receive                                                |  Transmit\n"
                   " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    for (int k = 0; k < 2; k++)
        if (net_iface(k))
            p += ksnprintf(p, (size_t)(end - p), "%6s: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", if_name(k));
    return text;
}

static const char *arp(void)
{
    char *p = text, *end = text + sizeof text;
    p += ksnprintf(p, (size_t)(end - p), "IP address       HW type     Flags       HW address            Mask     Device\n");
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip;
        struct netif *n;
        struct eth_addr *eth;
        if (!etharp_get_entry(i, &ip, &n, &eth))
            continue;
        uint32_t a = ip4_addr_get_u32(ip);
        char addr[16];
        ksnprintf(addr, sizeof addr, "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
        p += ksnprintf(p, (size_t)(end - p), "%s", addr);
        for (size_t l = strlen(addr); l < 17; l++)
            *p++ = ' ';
        const uint8_t *m = eth->addr;
        p += ksnprintf(p, (size_t)(end - p), "0x1         0x2         %02x:%02x:%02x:%02x:%02x:%02x     *        eth0\n",
                       m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return text;
}

static const char *snmp(void)
{
    ksnprintf(text, sizeof text,
              "Ip: Forwarding DefaultTTL InReceives InHdrErrors InAddrErrors ForwDatagrams InUnknownProtos InDiscards InDelivers OutRequests OutDiscards OutNoRoutes ReasmTimeout ReasmReqds ReasmOKs ReasmFails FragOKs FragFails FragCreates\n"
              "Ip: 2 64 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
              "Icmp: InMsgs InErrors InCsumErrors InDestUnreachs InTimeExcds InParmProbs InSrcQuenchs InRedirects InEchos InEchoReps InTimestamps InTimestampReps InAddrMasks InAddrMaskReps OutMsgs OutErrors OutRateLimitGlobal OutRateLimitHost OutDestUnreachs OutTimeExcds OutParmProbs OutSrcQuenchs OutRedirects OutEchos OutEchoReps OutTimestamps OutTimestampReps OutAddrMasks OutAddrMaskReps\n"
              "Icmp: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
              "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors\n"
              "Tcp: 1 200 120000 -1 0 0 0 0 0 0 0 0 0 0 0\n"
              "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors\n"
              "Udp: 0 0 0 0 0 0 0 0 0\n");
    return text;
}

/* Returns the text of one of the HAL's /proc files, or 0 if there is no such file. */
const char *proc_text(const char *path)
{
    for (size_t i = 0; i < sizeof sysctls / sizeof sysctls[0]; i++)
        if (!strcmp(path, sysctls[i].path))
            return sysctls[i].value;
    if (!strcmp(path, "/sys/devices/system/cpu/online") || !strcmp(path, "/sys/devices/system/cpu/possible")) {
        static char online[16];
        unsigned n = smp_count();
        if (n == 1)
            ksnprintf(online, sizeof online, "0\n");
        else
            ksnprintf(online, sizeof online, "0-%u\n", n - 1);
        return online;
    }
    if (!strcmp(path, "/proc/self/maps"))
        return vm_maps();
    if (!strcmp(path, "/proc/net/pnp"))
        return net_pnp();
    if (!strcmp(path, "/proc/net/tcp") || !strcmp(path, "/proc/net/udp")) {
        sock_proc_net(text, sizeof text, path[10] == 'u');
        return text;
    }
    if (!strcmp(path, "/proc/net/route"))
        return route();
    if (!strcmp(path, "/proc/net/dev"))
        return dev();
    if (!strcmp(path, "/proc/net/arp"))
        return arp();
    if (!strcmp(path, "/proc/net/snmp"))
        return snmp();
    return 0;
}
