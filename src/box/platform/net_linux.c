/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* net_linux.c -- the network, as far as the platform layer is concerned.
 *
 * Linux is the stack.  The kernel configures the interfaces itself when the
 * command line says ip=dhcp (CONFIG_IP_PNP_DHCP): ROSGD has no userland DHCP
 * client, and needs none.  What is left for the runtime is small:
 *
 *   - loopback up, which ip=dhcp would have done, for a box without it;
 *   - /etc/resolv.conf, pointing at /proc/net/pnp, where the kernel's DHCP
 *     writes the name servers it learned in resolv.conf's own format;
 *   - /etc/hosts, naming the loopback address localhost, as RISC OS's
 *     InetDBase:Hosts does.
 */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/platform.h"

int ros_net_init(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        strcpy(ifr.ifr_name, "lo");
        if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP)) {
            ifr.ifr_flags |= IFF_UP;
            ioctl(fd, SIOCSIFFLAGS, &ifr);
        }
        close(fd);
    }
    mkdir("/etc", 0755);
    if (access("/proc/net/pnp", R_OK) == 0)
        symlink("/proc/net/pnp", "/etc/resolv.conf");
    if (access("/etc/hosts", F_OK) != 0) {
        int h = open("/etc/hosts", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        static const char hosts[] = "127.0.0.1\tlocalhost\n::1\tlocalhost\n";
        if (h >= 0) {
            write(h, hosts, sizeof hosts - 1);
            close(h);
        }
    }

    int up = 0;
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0)
        return -errno;
    for (struct ifaddrs *a = all; a; a = a->ifa_next)
        up += a->ifa_addr && a->ifa_addr->sa_family == AF_INET && (a->ifa_flags & IFF_UP);
    freeifaddrs(all);
    return up;
}
