/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* hfstest.c: the HostFS test suite's guest executor, native.
 *
 * The team's HostFS suite (bigmacfarm tests/hostfs) drives a RISC OS
 * program, guest/hfstest.c, that runs scripts of filing-system primitives
 * and prints the raw answers; the host generates the scripts and judges the
 * answers.  ROSGD runs no ARM code, so this is that program ported: the
 * same ops, the same result lines to the character, the same --serve
 * protocol, and every call goes straight to the SWI it names.  Only the
 * plumbing differs.  What the SWIs see is in the arena.  The executor's own
 * files go through FileSwitch too, as the original's C library sent them.
 * They are the script, the results and the heartbeat.
 *
 * ros_hfstest_serve(dir) is the original's "hfstest --serve <dir>": /init
 * runs it with rosgd.hfstest on the kernel command line, the hosted binary
 * with ROSGD_HFTSERVE set (tests/hostfs/rosgd_suite.py).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define LINE_MAX_LEN 1024
#define MAX_FIELDS 16
#define SLOTS 64

static uint32_t slot_handle[SLOTS];
static unsigned long crc_table[256];

/* ---- calling ---------------------------------------------------------------- */

static os_error *sx(uint32_t n, struct ros_cpu *c)
{
    ros_swi(c, n);
    return c->v ? ros_ptr(c->r[0]) : NULL;
}

/* A C string in the arena, for a SWI: four rotating slots */
static uint32_t arena_str(const char *s)
{
    static uint32_t slots[4];
    static unsigned next;
    unsigned k = next++ & 3;
    if (!slots[k])
        slots[k] = ros_addr(ros_rma_alloc(LINE_MAX_LEN + 1));
    snprintf(ros_ptr(slots[k]), LINE_MAX_LEN + 1, "%s", s);
    return slots[k];
}

/* ---- the output: a file through FileSwitch, a line at a time ------------------- */

static uint32_t out_h;                  /* 0: the console */
static char line_buf[8192];
static size_t line_n;

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line_buf + line_n, sizeof line_buf - line_n, fmt, ap);
    va_end(ap);
    if (n > 0)
        line_n += (size_t)n < sizeof line_buf - line_n ? (size_t)n : sizeof line_buf - line_n - 1;
}

static void outc(char c)
{
    if (line_n < sizeof line_buf - 1)
        line_buf[line_n++] = c;
}

static void flush_out(void)
{
    if (!line_n)
        return;
    if (!out_h) {
        ros_console_write(line_buf, (uint32_t)line_n);
    } else {
        uint32_t b = ros_addr(ros_rma_alloc((uint32_t)line_n));
        memcpy(ros_ptr(b), line_buf, line_n);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 2, c.r[1] = out_h, c.r[2] = b, c.r[3] = (uint32_t)line_n;
        sx(XOS_GBPB, &c);
        ros_rma_free(ros_ptr(b));
    }
    line_n = 0;
}

/* ---- data (as the original) ---------------------------------------------------- */

static void crc_init(void)
{
    for (unsigned n = 0; n < 256; n++) {
        unsigned long c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned long crc_add(unsigned long crc, const unsigned char *p, unsigned long n)
{
    crc = crc ^ 0xFFFFFFFFUL;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

static unsigned char pattern(unsigned long seed, unsigned long k)
{
    return (unsigned char)((((seed + k) * 2654435761UL) & 0xFFFFFFFFUL) >> 24);
}

static void fill(unsigned char *buf, unsigned long n, unsigned long seed)
{
    for (unsigned long k = 0; k < n; k++)
        buf[k] = pattern(seed, k);
}

/* ---- printing (as the original) ------------------------------------------------ */

static void print_chars(const char *s, int keep_spaces)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p >= 0x7F || *p == '\\' || *p == '|' || *p == ':' ||
            (*p == ' ' && !keep_spaces))
            outf("\\x%02X", *p);
        else
            outc((char)*p);
    }
}

#define print_text(s) print_chars((s), 0)
#define print_message(s) print_chars((s), 1)

static void print_error(const char *id, const os_error *e)
{
    outf("%s err num=%X msg=", id, e->errnum);
    print_message(e->errmess);
    outf("\n");
}

/* ---- parsing (as the original) ------------------------------------------------- */

static long num(const char *s)
{
    if (s == NULL)
        return 0;
    if (s[0] == '#')
        return (long)strtoul(s + 1, NULL, 16);
    return strtol(s, NULL, 10);
}

static int split(char *line, char **field)
{
    int n = 0;
    char *p = line;
    while (n < MAX_FIELDS) {
        field[n++] = p;
        p = strchr(p, '\t');
        if (p == NULL)
            break;
        *p++ = '\0';
    }
    return n;
}

static int slot(const char *s)
{
    long k = num(s);
    return (k >= 0 && k < SLOTS) ? (int)k : 0;
}

static uint32_t handle_in(const char *id, const char *s)
{
    uint32_t h = slot_handle[slot(s)];
    if (h == 0)
        outf("%s err num=0 msg=no handle in slot %s\n", id, s);
    return h;
}

/* ---- the heartbeat ------------------------------------------------------------- */

static char alive_path[268];
static int serving, beats;
static uint32_t last_beat;

static uint32_t now_cs(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    sx(XOS_ReadMonotonicTime, &c);
    return c.r[0];
}

static void heartbeat(int force)
{
    if (!serving)
        return;
    if (!force && now_cs() - last_beat < 100)
        return;
    last_beat = now_cs();
    /* Its own arena buffers: a beat comes in the middle of an op, whose
     * names are in arena_str's slots */
    static uint32_t path, text;
    if (!path) {
        path = ros_addr(ros_rma_alloc(sizeof alive_path));
        text = ros_addr(ros_rma_alloc(16));
    }
    strcpy(ros_ptr(path), alive_path);
    int n = snprintf(ros_ptr(text), 16, "%d\n", ++beats);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = path, c.r[2] = 0xFFF, c.r[4] = text, c.r[5] = text + (uint32_t)n;
    sx(XOS_File, &c);
}

/* ---- the operations (as the original) -------------------------------------------- */

static void op_find(const char *id, char **f, int n)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = (uint32_t)num(f[3]), c.r[1] = arena_str(n > 4 ? f[4] : "");
    os_error *e = sx(XOS_Find, &c);
    if (e) {
        print_error(id, e);
        return;
    }
    slot_handle[slot(f[2])] = c.r[0];
    outf("%s ok h=%d\n", id, (int)c.r[0]);
}

static void op_close(const char *id, char **f)
{
    int k = slot(f[2]);
    if (handle_in(id, f[2]) == 0)
        return;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = slot_handle[k];
    os_error *e = sx(XOS_Find, &c);
    if (e) {
        print_error(id, e);
        return;
    }
    slot_handle[k] = 0;
    outf("%s ok\n", id);
}

static void op_closeall(const char *id)
{
    int closed = 0;
    for (int k = 0; k < SLOTS; k++)
        if (slot_handle[k]) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = 0, c.r[1] = slot_handle[k];
            sx(XOS_Find, &c);
            slot_handle[k] = 0;
            closed++;
        }
    outf("%s ok closed=%d\n", id, closed);
}

static void op_bput(const char *id, char **f)
{
    uint32_t h = handle_in(id, f[2]);
    unsigned long count = (unsigned long)num(f[3]), seed = (unsigned long)num(f[4]), k;
    if (h == 0)
        return;
    for (k = 0; k < count; k++) {
        if ((k & 0x3FFF) == 0)
            heartbeat(0);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = pattern(seed, k), c.r[1] = h;
        os_error *e = sx(XOS_BPut, &c);
        if (e) {
            outf("%s err at=%lu num=%X msg=", id, k, e->errnum);
            print_message(e->errmess);
            outf("\n");
            return;
        }
    }
    outf("%s ok n=%lu\n", id, count);
}

static void op_bget(const char *id, char **f)
{
    uint32_t h = handle_in(id, f[2]);
    unsigned long count = (unsigned long)num(f[3]), k, crc = 0;
    int eof = 0;
    char head[16 * 2 + 1];
    if (h == 0)
        return;
    head[0] = '\0';
    for (k = 0; k < count; k++) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = h;
        os_error *e = sx(XOS_BGet, &c);
        if (e) {
            outf("%s err at=%lu num=%X msg=", id, k, e->errnum);
            print_message(e->errmess);
            outf("\n");
            return;
        }
        if (c.c) {
            eof = 1;
            break;
        }
        unsigned char b = (unsigned char)c.r[0];
        crc = crc_add(crc, &b, 1);
        if (k < 16)
            sprintf(head + 2 * k, "%02X", b);
        if ((k & 0x3FFF) == 0)
            heartbeat(0);
    }
    outf("%s ok n=%lu eof=%d crc=%08lX head=%s\n", id, k, eof, crc, head);
}

static void op_gbpb(const char *id, char **f, int n)
{
    int reason = (int)num(f[2]);
    uint32_t h = handle_in(id, f[3]);
    unsigned long count = (unsigned long)num(f[4]), seed = (unsigned long)num(f[5]);
    uint32_t ptr = n > 6 ? (uint32_t)num(f[6]) : 0;
    if (h == 0)
        return;
    uint32_t buf = ros_addr(ros_rma_alloc((uint32_t)(count ? count : 1)));
    if (!buf) {
        outf("%s err num=0 msg=no memory for %lu bytes\n", id, count);
        return;
    }
    if (reason == 1 || reason == 2)
        fill(ros_ptr(buf), count, seed);
    else
        memset(ros_ptr(buf), 0xA5, count);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = (uint32_t)reason, c.r[1] = h, c.r[2] = buf, c.r[3] = (uint32_t)count, c.r[4] = ptr;
    os_error *e = sx(XOS_GBPB, &c);
    if (e) {
        ros_rma_free(ros_ptr(buf));
        print_error(id, e);
        return;
    }
    unsigned long crc = (reason == 3 || reason == 4)
                            ? crc_add(0, ros_ptr(buf), count - c.r[3]) : 0;
    outf("%s ok r2=%X r3=%d r4=%d crc=%08lX\n", id, c.r[2] - buf, (int)c.r[3], (int)c.r[4],
         crc);
    ros_rma_free(ros_ptr(buf));
}

static void op_args(const char *id, char **f, int n)
{
    uint32_t h = handle_in(id, f[3]);
    if (h == 0)
        return;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = (uint32_t)num(f[2]), c.r[1] = h, c.r[2] = n > 4 ? (uint32_t)num(f[4]) : 0;
    os_error *e = sx(XOS_Args, &c);
    if (e) {
        print_error(id, e);
        return;
    }
    outf("%s ok r0=%X r2=%d\n", id, c.r[0], (int)c.r[2]);
}

static void op_file(const char *id, char **f, int n)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = (uint32_t)num(f[2]), c.r[1] = arena_str(f[3]);
    c.r[2] = n > 4 ? (uint32_t)num(f[4]) : 0, c.r[3] = n > 5 ? (uint32_t)num(f[5]) : 0;
    c.r[4] = n > 6 ? (uint32_t)num(f[6]) : 0, c.r[5] = n > 7 ? (uint32_t)num(f[7]) : 0;
    c.r[6] = 0;
    os_error *e = sx(XOS_File, &c);
    if (e) {
        print_error(id, e);
        return;
    }
    outf("%s ok r0=%X r2=%X r3=%X r4=%X r5=%X r6=%X\n", id, c.r[0], c.r[2], c.r[3], c.r[4],
         c.r[5], c.r[6]);
}

static void op_save(const char *id, char **f)
{
    unsigned long count = (unsigned long)num(f[4]);
    uint32_t buf = ros_addr(ros_rma_alloc((uint32_t)(count ? count : 1)));
    if (!buf) {
        outf("%s err num=0 msg=no memory for %lu bytes\n", id, count);
        return;
    }
    fill(ros_ptr(buf), count, (unsigned long)num(f[5]));
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = arena_str(f[2]), c.r[2] = (uint32_t)num(f[3]), c.r[4] = buf,
    c.r[5] = buf + (uint32_t)count;
    os_error *e = sx(XOS_File, &c);
    ros_rma_free(ros_ptr(buf));
    if (e) {
        print_error(id, e);
        return;
    }
    outf("%s ok n=%lu\n", id, count);
}

static void op_savele(const char *id, char **f)
{
    unsigned long count = (unsigned long)num(f[5]);
    uint32_t buf = ros_addr(ros_rma_alloc((uint32_t)(count ? count : 1)));
    if (!buf) {
        outf("%s err num=0 msg=no memory for %lu bytes\n", id, count);
        return;
    }
    fill(ros_ptr(buf), count, (unsigned long)num(f[6]));
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = arena_str(f[2]), c.r[2] = (uint32_t)num(f[3]),
    c.r[3] = (uint32_t)num(f[4]), c.r[4] = buf, c.r[5] = buf + (uint32_t)count;
    os_error *e = sx(XOS_File, &c);
    ros_rma_free(ros_ptr(buf));
    if (e) {
        print_error(id, e);
        return;
    }
    outf("%s ok n=%lu\n", id, count);
}

static void op_dup(const char *id, char **f)
{
    slot_handle[slot(f[3])] = slot_handle[slot(f[2])];
    outf("%s ok h=%d\n", id, (int)slot_handle[slot(f[3])]);
}

static void op_load(const char *id, char **f)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 5, c.r[1] = arena_str(f[2]);
    os_error *e = sx(XOS_File, &c);
    if (e) {
        print_error(id, e);
        return;
    }
    uint32_t type = c.r[0], len = c.r[4];
    if (type != 1) {
        outf("%s ok obj=%d\n", id, (int)type);
        return;
    }
    uint32_t buf = ros_addr(ros_rma_alloc(len ? len : 1));
    if (!buf) {
        outf("%s err num=0 msg=no memory for %d bytes\n", id, (int)len);
        return;
    }
    ros_cpu_enter(&c);
    c.r[0] = 255, c.r[1] = arena_str(f[2]), c.r[2] = buf, c.r[3] = 0;
    if ((e = sx(XOS_File, &c)) != NULL) {
        ros_rma_free(ros_ptr(buf));
        print_error(id, e);
        return;
    }
    outf("%s ok obj=1 len=%d crc=%08lX\n", id, (int)len, crc_add(0, ros_ptr(buf), len));
    ros_rma_free(ros_ptr(buf));
}

static void op_crcfile(const char *id, char **f)
{
    unsigned long chunk = (unsigned long)num(f[3]), total = 0, crc = 0;
    if (chunk == 0)
        chunk = 4096;
    uint32_t buf = ros_addr(ros_rma_alloc((uint32_t)chunk));
    if (!buf) {
        outf("%s err num=0 msg=no memory\n", id);
        return;
    }
    uint32_t h = 0, ext = 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x4F, c.r[1] = arena_str(f[2]);
    os_error *e = sx(XOS_Find, &c);
    if (!e) {
        h = c.r[0];
        ros_cpu_enter(&c);
        c.r[0] = 2, c.r[1] = h;
        e = sx(XOS_Args, &c);
        ext = c.r[2];
    }
    while (!e) {
        heartbeat(0);
        ros_cpu_enter(&c);
        c.r[0] = 4, c.r[1] = h, c.r[2] = buf, c.r[3] = (uint32_t)chunk;
        if ((e = sx(XOS_GBPB, &c)) != NULL)
            break;
        crc = crc_add(crc, ros_ptr(buf), chunk - c.r[3]);
        total += chunk - c.r[3];
        if (c.r[3] != 0)
            break;
    }
    if (h) {
        struct ros_cpu k;
        ros_cpu_enter(&k);
        k.r[0] = 0, k.r[1] = h;
        sx(XOS_Find, &k);
    }
    ros_rma_free(ros_ptr(buf));
    if (e) {
        print_error(id, e);
        return;
    }
    outf("%s ok len=%lu ext=%d crc=%08lX\n", id, total, (int)ext, crc);
}

static void op_fsc(const char *id, char **f, int n)
{
    int reason = (int)num(f[2]);
    struct ros_cpu c;
    os_error *e;
    ros_cpu_enter(&c);
    switch (reason) {
    case 37: {
        uint32_t buf = ros_addr(ros_rma_alloc(512));
        ros_st8(buf, 0);
        c.r[0] = 37, c.r[1] = arena_str(f[3]), c.r[2] = buf, c.r[3] = 0, c.r[4] = 0,
        c.r[5] = 512;
        e = sx(XOS_FSControl, &c);
        if (!e) {
            outf("%s ok spare=%d name=", id, (int)c.r[5]);
            print_text(ros_ptr(buf));
            outf("\n");
        }
        ros_rma_free(ros_ptr(buf));
        if (!e)
            return;
        break;
    }
    case 49:
    case 55:
        c.r[0] = (uint32_t)reason, c.r[1] = arena_str(f[3]);
        e = sx(XOS_FSControl, &c);
        if (!e) {
            outf("%s ok r0=%X r1=%X r2=%X r3=%X r4=%X\n", id, c.r[0], c.r[1], c.r[2], c.r[3],
                 c.r[4]);
            return;
        }
        break;
    default:
        c.r[0] = (uint32_t)reason, c.r[1] = arena_str(f[3]);
        c.r[2] = arena_str(n > 4 ? f[4] : "");
        c.r[3] = n > 5 ? (uint32_t)num(f[5]) : 0;
        c.r[4] = c.r[5] = c.r[6] = c.r[7] = c.r[8] = 0;
        e = sx(XOS_FSControl, &c);
        if (!e) {
            outf("%s ok\n", id);
            return;
        }
        break;
    }
    print_error(id, e);
}

static long bounded_strlen(const unsigned char *s, const unsigned char *end)
{
    const unsigned char *p = s;
    while (p < end && *p != 0)
        p++;
    return (p < end) ? (long)(p - s) : -1L;
}

static void op_enum(const char *id, char **f, int n)
{
    int quiet = n > 7 && f[7][0] == 'q';
    int reason = (int)num(f[2]);
    const char *wild = f[4];
    int per_call = (int)num(f[5]);
    int buflen = (int)num(f[6]);
    int nameat = reason == 9 ? 0 : reason == 10 ? 20 : reason == 11 ? 29 : 24;
    int nwords = reason == 12 ? 6 : 5;
    uint32_t abuf = ros_addr(ros_rma_alloc((uint32_t)(buflen > 0 ? buflen : 1)));
    if (!abuf) {
        outf("%s err num=0 msg=no memory\n", id);
        return;
    }
    unsigned char *buf = ros_ptr(abuf), *end = buf + (buflen > 0 ? buflen : 1);
    uint32_t dir = arena_str(f[3]), wa = wild[0] ? arena_str(wild) : 0;
    int offset = 0, got = 0, calls = 0, count = 0;
    unsigned long names_crc = 0;
    os_error *e = NULL;
    outf("%s ok list=", id);
    if (quiet)
        outf("-");
    while (calls < 100000) {
        calls++;
        heartbeat(0);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = (uint32_t)reason, c.r[1] = dir, c.r[2] = abuf, c.r[3] = (uint32_t)per_call,
        c.r[4] = (uint32_t)offset, c.r[5] = (uint32_t)buflen, c.r[6] = wa;
        if ((e = sx(XOS_GBPB, &c)) != NULL)
            break;
        got = (int)c.r[3];
        offset = (int)c.r[4];
        unsigned char *p = buf;
        for (int i = 0; i < got; i++) {
            if (reason != 9 && p + nameat >= end)
                break;
            const unsigned char *name = p + nameat;
            long len = bounded_strlen(name, end);
            if (len < 0)
                break;
            if (count && !quiet)
                outc('|');
            if (!quiet) {
                print_text((const char *)name);
                if (reason != 9)
                    for (int k = 0; k < nwords; k++)
                        outf(":%X", ros_ld32(abuf + (uint32_t)(p - buf) + 4u * (uint32_t)k));
            }
            names_crc = crc_add(names_crc, name, (unsigned long)len + 1UL);
            count++;
            if (reason == 9) {
                p = (unsigned char *)name + len + 1;
            } else {
                unsigned long rec = (unsigned long)nameat + (unsigned long)len + 1UL;
                p += (rec + 3UL) & ~3UL;
            }
            if (p > end)
                break;
        }
        if (offset == -1)
            break;
        if (got == 0 && calls > 1000)
            break;
    }
    ros_rma_free(ros_ptr(abuf));
    if (e) {
        outf(" count=%d calls=%d num=%X msg=", count, calls, e->errnum);
        print_message(e->errmess);
        outf("\n");
        return;
    }
    outf(" count=%d calls=%d crc=%08lX end=%d\n", count, calls, names_crc, offset);
}

static void op_cli(const char *id, char **f)
{
    uint32_t line = arena_str(f[2]);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = line;
    os_error *e = sx(XOS_CLI, &c);
    if (e) {
        print_error(id, e);
        return;
    }
    outf("%s ok\n", id);
}

static void op_time(const char *id)
{
    uint32_t block = arena_str("");
    ros_st8(block, 3);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 14, c.r[1] = block;
    sx(XOS_Word, &c);
    outf("%s ok t=%02X%02X%02X%02X%02X\n", id, ros_ld8(block + 4), ros_ld8(block + 3),
         ros_ld8(block + 2), ros_ld8(block + 1), ros_ld8(block));
}

static void op_wait(const char *id, char **f)
{
    uint32_t until = now_cs() + (uint32_t)num(f[2]);
    while ((int32_t)(now_cs() - until) < 0) {
        heartbeat(0);
        usleep(2000);
    }
    outf("%s ok waited=%s\n", id, f[2]);
}

/* One script, to the end */
static void run_script(char *text)
{
    char *field[MAX_FIELDS];
    memset(slot_handle, 0, sizeof slot_handle);
    outf("0 ok hfstest=2\n");
    flush_out();
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == '\n'))
            line[--len] = 0;
        if (len == 0 || line[0] == '#') {
            line = next;
            continue;
        }
        if (len >= LINE_MAX_LEN)
            line[LINE_MAX_LEN - 1] = 0;
        int n = split(line, field);
        if (n >= 2) {
            const char *op = field[1];
            if (!strcmp(op, "find") && n >= 4) op_find(field[0], field, n);
            else if (!strcmp(op, "close") && n >= 3) op_close(field[0], field);
            else if (!strcmp(op, "closeall")) op_closeall(field[0]);
            else if (!strcmp(op, "bput") && n >= 5) op_bput(field[0], field);
            else if (!strcmp(op, "bget") && n >= 4) op_bget(field[0], field);
            else if (!strcmp(op, "gbpb") && n >= 6) op_gbpb(field[0], field, n);
            else if (!strcmp(op, "args") && n >= 4) op_args(field[0], field, n);
            else if (!strcmp(op, "file") && n >= 4) op_file(field[0], field, n);
            else if (!strcmp(op, "save") && n >= 6) op_save(field[0], field);
            else if (!strcmp(op, "load") && n >= 3) op_load(field[0], field);
            else if (!strcmp(op, "crcfile") && n >= 4) op_crcfile(field[0], field);
            else if (!strcmp(op, "fsc") && n >= 4) op_fsc(field[0], field, n);
            else if (!strcmp(op, "enum") && n >= 7) op_enum(field[0], field, n);
            else if (!strcmp(op, "savele") && n >= 7) op_savele(field[0], field);
            else if (!strcmp(op, "dup") && n >= 4) op_dup(field[0], field);
            else if (!strcmp(op, "cli") && n >= 3) op_cli(field[0], field);
            else if (!strcmp(op, "time")) op_time(field[0]);
            else if (!strcmp(op, "wait") && n >= 3) op_wait(field[0], field);
            else outf("%s err num=0 msg=bad script line\n", field[0]);
            flush_out();
            heartbeat(0);
        }
        line = next;
    }
    for (int k = 0; k < SLOTS; k++)
        if (slot_handle[k]) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = 0, c.r[1] = slot_handle[k];
            sx(XOS_Find, &c);
            slot_handle[k] = 0;
        }
    outf("END ok\n");
    flush_out();
}

static uint32_t object_type(const char *path)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17, c.r[1] = arena_str(path);
    return sx(XOS_File, &c) ? 0 : c.r[0];
}

/* A whole file, through FileSwitch, as a C string; NULL if it will not load */
static char *load_text(const char *path)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 5, c.r[1] = arena_str(path);
    if (sx(XOS_File, &c) || c.r[0] != 1)
        return NULL;
    uint32_t len = c.r[4];
    uint32_t buf = ros_addr(ros_rma_alloc(len + 1));
    if (!buf)
        return NULL;
    ros_cpu_enter(&c);
    c.r[0] = 255, c.r[1] = arena_str(path), c.r[2] = buf, c.r[3] = 0;
    char *text = NULL;
    if (!sx(XOS_File, &c) && (text = malloc(len + 1)) != NULL) {
        memcpy(text, ros_ptr(buf), len);
        text[len] = 0;
    }
    ros_rma_free(ros_ptr(buf));
    return text;
}

int ros_hfstest_serve(const char *dir)
{
    char q[256], stop[256], path[512], rpath[512];
    crc_init();
    snprintf(q, sizeof q, "%s.q", dir);
    snprintf(alive_path, sizeof alive_path, "%s.alive", dir);
    snprintf(stop, sizeof stop, "%s.stop", dir);
    serving = 1;
    ros_console_printf("rosgd: hfstest serving %s\n", dir);
    uint32_t name = ros_addr(ros_rma_alloc(64));
    for (;;) {
        if (object_type(stop) != 0)
            return 0;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 9, c.r[1] = arena_str(q), c.r[2] = name, c.r[3] = 1, c.r[4] = 0, c.r[5] = 64,
        c.r[6] = 0;
        ros_st8(name, 0);
        uint32_t read = sx(XOS_GBPB, &c) ? 0 : c.r[3];
        if (read > 0 && ros_ld8(name)) {
            char leaf[64];
            snprintf(leaf, sizeof leaf, "%s", (const char *)ros_ptr(name));
            snprintf(path, sizeof path, "%s.q.%s", dir, leaf);
            snprintf(rpath, sizeof rpath, "%s.r.%s", dir, leaf);
            char *script = load_text(path);
            ros_cpu_enter(&c);
            c.r[0] = 0x80, c.r[1] = arena_str(rpath);
            out_h = sx(XOS_Find, &c) ? 0 : c.r[0];
            if (script && out_h)
                run_script(script);
            free(script);
            if (out_h) {
                ros_cpu_enter(&c);
                c.r[0] = 0, c.r[1] = out_h;
                sx(XOS_Find, &c);
                out_h = 0;
            }
            snprintf(rpath, sizeof rpath, "%s.d.%s", dir, leaf);
            ros_cpu_enter(&c);
            c.r[0] = 10, c.r[1] = arena_str(rpath), c.r[2] = 0xFFD, c.r[4] = 0, c.r[5] = 0;
            sx(XOS_File, &c);
            ros_cpu_enter(&c);
            c.r[0] = 6, c.r[1] = arena_str(path);
            sx(XOS_File, &c);
            continue;
        }
        heartbeat(0);
        usleep(100000);
    }
}
