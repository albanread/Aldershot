/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_resolver.c: the Resolver, over the host's name lookup, against
 * the behaviour read from the original's code (modules/resolver/README.md).
 *
 * The names used need no network: localhost, a dotted address, and a name
 * under .invalid, which never exists. So it runs the same hosted. With
 * rosgd.dnstest=NAME on the kernel command line it also looks NAME up
 * through the name server the kernel's DHCP found.
 *
 * Hostents are read as a RISC OS client reads them: words in the RMA.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

/* The first IPv4 address of a hostent, as a word, or 0. */
static uint32_t first_addr(uint32_t h)
{
    if (!h || ros_ld32(h + 8) != 2 || ros_ld32(h + 12) != 4)
        return 0;
    uint32_t list = ros_ld32(h + 16), a = list ? ros_ld32(list) : 0;
    return a ? ros_ld32(a) : 0;
}

static const char *hname(uint32_t h)
{
    return h && ros_ld32(h) ? (const char *)ros_ptr(ros_ld32(h)) : "";
}

#define IP(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

/* GetHost, polled as a Wimp program polls it, for up to about 5 s. */
static os_error *poll_host(const char *name, uint32_t *status, uint32_t *h, unsigned *polls)
{
    char *copy = ros_rma_alloc((uint32_t)strlen(name) + 1);
    strcpy(copy, name);
    os_error *e = NULL;
    *polls = 0;
    uint32_t start = ros_time_cs();
    do {
        e = xresolver_get_host(ros_addr(copy), 0, status, h);
        ++*polls;
    } while (!e && *status == 36 && ros_time_cs() - start < 500);
    ros_rma_free(copy);
    return e;
}

void ros_selftest_resolver(void)
{
    uint32_t status = 99, h = 0;
    unsigned polls;
    os_error *e;

    /* ---- blocking, through the SWI chunk ---- */
    char *name = ros_rma_alloc(64);
    strcpy(name, "localhost");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = ros_addr(name);            /* inetlib passes R0 = 0 */
    ros_swi(&s, XResolver_GetHostByName);
    check(!s.v && s.r[0] == 0 && first_addr(s.r[1]) == IP(127, 0, 0, 1),
          "Resolver: GetHostByName localhost -- a hostent for 127.0.0.1",
          "V %u, R0 %u, R1 &%08X", s.v, s.r[0], s.r[1]);
    h = s.r[1];
    check(h && ros_ld32(h + 4) && ros_ld32(ros_ld32(h + 4)) == 0 &&
              ros_ld32(ros_ld32(h + 16) + 4) == 0 && ros_in_arena(ros_ptr(h)),
          "Resolver: the hostent is RISC OS's -- in the RMA, lists ending in 0", NULL);
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = ros_addr(name);
    ros_swi(&s, XResolver_GetHostByName);
    check(!s.v && s.r[1] == h, "Resolver: a second lookup comes from the cache", NULL);

    e = xresolver_get_host_by_name("10.0.2.15", &status, &h);
    check(!e && status == 0 && first_addr(h) == IP(10, 0, 2, 15),
          "Resolver: GetHostByName of a dotted address", "%s", e ? e->errmess : "ok");

    /* With a name server, a failure status; without, "No DNS service". */
    e = xresolver_get_host_by_name("no-such-host.invalid", &status, &h);
    check((!e && status != 0 && status != 36 && h == 0) || (e && e->errnum == 0x818042u),
          "Resolver: a name that does not exist -- R1 0 and a failure status, or no DNS",
          "%s status %u", e ? e->errmess : "", status);

    e = xresolver_get_host_by_name("bad name!", &status, &h);
    check(e && e->errnum == 0x818040u,
          "Resolver: a bad character -- \"Bad parameters\", &818040", "&%X",
          e ? e->errnum : 0);

    /* ---- non-blocking: R0 names the host, and callers poll ---- */
    xresolver_cache_control(0);
    char *copy = ros_rma_alloc(32);
    strcpy(copy, "localhost");
    e = xresolver_get_host(ros_addr(copy), 0, &status, &h);
    check(!e && status == 0 && first_addr(h) == IP(127, 0, 0, 1),
          "Resolver: GetHost answers a hosts-file name at once", "status %u", status);
    strcpy(copy, "another.invalid");
    e = xresolver_get_host(ros_addr(copy), 0, &status, &h);
    check((!e && status == 36 && h == 0) || (e && e->errnum == 0x818042u),
          "Resolver: GetHost of a DNS name starts a lookup -- 36, or no DNS",
          "%s status %u", e ? e->errmess : "", status);
    ros_rma_free(copy);
    e = poll_host("another.invalid", &status, &h, &polls);
    check((!e && status != 0 && status != 36 && h == 0) || (e && e->errnum == 0x818042u),
          "Resolver: polled, a name that does not exist fails", "%s status %u after %u polls",
          e ? e->errmess : "", status, polls);

    /* ---- by address: R0 = 0, R1 -> four bytes ---- */
    uint32_t *addr = ros_rma_alloc(4);
    *addr = IP(127, 0, 0, 1);
    e = xresolver_get_host(0, ros_addr(addr), &status, &h);
    check(!e && status == 0 && strncmp(hname(h), "localhost", 9) == 0 &&
              first_addr(h) == IP(127, 0, 0, 1),
          "Resolver: GetHost with R0 = 0 -- 127.0.0.1 is localhost", "status %u, \"%s\"",
          status, hname(h));
    ros_rma_free(addr);

    e = xresolver_cache_control(0);
    check(!e, "Resolver: CacheControl 0 flushes the cache", NULL);

    /* ---- a real name, through the DHCP server's name server, if asked ---- */
    const char *want = strstr(ros_cmdline(), "rosgd.dnstest=");
    if (want) {
        char real[64];
        sscanf(want + 14, "%63s", real);
        e = poll_host(real, &status, &h, &polls);
        uint32_t a = first_addr(h);
        check(!e && status == 0 && a,
              "Resolver: a name from the network's name server", "%s: status %u", real, status);
        if (a)
            ros_console_printf("        %s is %s, %u.%u.%u.%u, after %u polls\n", real, hname(h),
                               a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24, polls);
    }
    ros_rma_free(name);
}
