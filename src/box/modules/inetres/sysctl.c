/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1993
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
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's sysctl(8)
 * as ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/sysctl: c.SysCtl).
 */

/* sysctl.c -- *SysCtl (InetRes/Sources/sysctl, FreeBSD's sysctl), over
 * Linux's /proc/sys.
 *
 * It uses FreeBSD's names, each mapped to Linux's variable where there is
 * one. net.inet.ip.forwarding is /proc/sys/net/ipv4/ip_forward, and so on
 * down the table. "name: value" reads (-n shows the value alone, -N the
 * name alone), -a reads every name, and "name=value" sets, printing
 * "name: old -> new". The setting is quiet with -e, as RISC OS's is, which
 * sends errors to Inet$Error. A name that the table does not have gives
 * FreeBSD's "unknown oid" (-i ignores it). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inetres.h"

struct oid {
    const char *name, *path;
    int field;                      /* which number of the file's (0 the first) */
    int scale;                      /* FreeBSD's value is Linux's times this (ms for s) */
    int invert;                     /* a boolean Linux holds the other way round */
    const char *descr;
};

static const struct oid oids[] = {
    { "kern.hostname", "/proc/sys/kernel/hostname", -1, 1, 0, "Hostname" },
    { "net.inet.ip.forwarding", "/proc/sys/net/ipv4/ip_forward", 0, 1, 0, "Enable IP forwarding between interfaces" },
    { "net.inet.ip.ttl", "/proc/sys/net/ipv4/ip_default_ttl", 0, 1, 0, "Maximum TTL on IP packets" },
    { "net.inet.ip.portrange.first", "/proc/sys/net/ipv4/ip_local_port_range", 0, 1, 0, "" },
    { "net.inet.ip.portrange.last", "/proc/sys/net/ipv4/ip_local_port_range", 1, 1, 0, "" },
    { "net.inet.icmp.bmcastecho", "/proc/sys/net/ipv4/icmp_echo_ignore_broadcasts", 0, 1, 1,
      "Reply to multicast/broadcast ICMP echo requests" },
    { "net.inet.tcp.sendspace", "/proc/sys/net/ipv4/tcp_wmem", 1, 1, 0, "Initial send socket buffer size" },
    { "net.inet.tcp.recvspace", "/proc/sys/net/ipv4/tcp_rmem", 1, 1, 0, "Initial receive socket buffer size" },
    { "net.inet.tcp.keepidle", "/proc/sys/net/ipv4/tcp_keepalive_time", 0, 1000, 0,
      "time before keepalive probes begin" },
    { "net.inet.tcp.keepintvl", "/proc/sys/net/ipv4/tcp_keepalive_intvl", 0, 1000, 0,
      "time between keepalive probes" },
    { "net.inet.tcp.rfc1323", "/proc/sys/net/ipv4/tcp_timestamps", 0, 1, 0,
      "Enable rfc1323 (high performance TCP) extensions" },
    { "net.inet.tcp.syncookies", "/proc/sys/net/ipv4/tcp_syncookies", 0, 1, 0, "Use TCP SYN cookies" },
};

static int read_file(const char *path, char *out, size_t max)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(out, 1, max - 1, f);
    fclose(f);
    out[n] = 0;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

/* The value FreeBSD shows, from Linux's file. */
static int get(const struct oid *o, char *out, size_t max)
{
    char raw[256];
    if (read_file(o->path, raw, sizeof raw))
        return -1;
    if (o->field < 0) {
        snprintf(out, max, "%s", raw);
        return 0;
    }
    char *save, *w = strtok_r(raw, " \t", &save);
    for (int i = 0; w && i < o->field; i++)
        w = strtok_r(NULL, " \t", &save);
    if (!w)
        return -1;
    long v = strtol(w, NULL, 10);
    if (o->invert)
        v = !v;
    snprintf(out, max, "%ld", v * o->scale);
    return 0;
}

static int set(const struct oid *o, const char *value)
{
    char line[256];
    if (o->field < 0) {
        snprintf(line, sizeof line, "%s\n", value);
    } else {
        long v = strtol(value, NULL, 10) / o->scale;
        if (o->invert)
            v = !v;
        char raw[256];
        if (read_file(o->path, raw, sizeof raw))
            return -1;
        char *parts[4] = { 0 }, *save, *w = strtok_r(raw, " \t", &save);
        int n = 0;
        for (; w && n < 4; w = strtok_r(NULL, " \t", &save))
            parts[n++] = w;
        char num[24];
        snprintf(num, sizeof num, "%ld", v);
        if (o->field >= n)
            return -1;
        parts[o->field] = num;
        size_t k = 0;
        for (int i = 0; i < n; i++)
            k += (size_t)snprintf(line + k, sizeof line - k, "%s%s", i ? "\t" : "", parts[i]);
        snprintf(line + k, sizeof line - k, "\n");
    }
    FILE *f = fopen(o->path, "w");
    if (!f)
        return -1;
    int ok = fputs(line, f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

os_error *inet_sysctl(const struct inet_args *a)
{
    struct inet_tool t = { "sysctl", 0 };
    struct inet_opt o = { 0 };
    int all = 0, value_only = 0, name_only = 0, ignore = 0, descr = 0, c;
    while ((c = inet_getopt(&o, a, "abdeiNnoqtWxB:f:")) != -1) {
        switch (c) {
        case 'a':
            all = 1;
            break;
        case 'd':
            descr = 1;
            break;
        case 'e':
            t.e_flag = 1;
            break;
        case 'i':
            ignore = 1;
            break;
        case 'n':
            value_only = 1;
            break;
        case 'N':
            name_only = 1;
            break;
        case 'q':
        case 'o':
        case 't':
        case 'W':
        case 'x':
            break;
        case '?':
            return inet_fail(&t, 1, "illegal option -- %c", a->argv[o.ind - 1][1]);
        default:
            return inet_fail(&t, 1, "option -%c is not carried by ROSGD's sysctl", c);
        }
    }
    const unsigned n = sizeof oids / sizeof oids[0];
    os_error *e = NULL;
    if (all || o.ind >= a->argc) {
        if (!all)
            return inet_printf("Usage:   sysctl [-deiNn] <variable>[=value] ...\n         sysctl [-deNn] -a\n");
        for (unsigned i = 0; !e && i < n; i++) {
            char v[256];
            if (get(&oids[i], v, sizeof v))
                continue;
            if (name_only)
                e = inet_printf("%s\n", oids[i].name);
            else if (value_only)
                e = inet_printf("%s\n", descr ? oids[i].descr : v);
            else
                e = inet_printf("%s: %s\n", oids[i].name, descr ? oids[i].descr : v);
        }
        return e;
    }
    for (int k = o.ind; !e && k < a->argc; k++) {
        char name[128];
        snprintf(name, sizeof name, "%s", a->argv[k]);
        char *eq = strchr(name, '=');
        const char *value = NULL;
        if (eq) {
            *eq = 0;
            value = eq + 1;
        }
        const struct oid *hit = NULL;
        for (unsigned i = 0; i < n; i++)
            if (!strcmp(oids[i].name, name))
                hit = &oids[i];
        if (!hit) {
            if (ignore)
                continue;
            return inet_fail(&t, 1, "unknown oid '%s'", name);
        }
        char old[256];
        if (get(hit, old, sizeof old))
            return inet_fail(&t, 1, "%s: %s", name, strerror(errno));
        if (value) {
            if (set(hit, value))
                return inet_fail(&t, 1, "%s: %s", name, strerror(errno ? errno : EINVAL));
            char now[256];
            get(hit, now, sizeof now);
            if (!t.e_flag)
                e = value_only ? inet_printf("%s\n", now) : inet_printf("%s: %s -> %s\n", name, old, now);
            continue;
        }
        if (name_only)
            e = inet_printf("%s\n", name);
        else if (value_only)
            e = inet_printf("%s\n", descr ? hit->descr : old);
        else
            e = inet_printf("%s: %s\n", name, descr ? hit->descr : old);
    }
    return e;
}
