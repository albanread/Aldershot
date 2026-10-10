/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_filterhourglass.c: the Filter Manager and the Hourglass, both
 * compiled from ObjAsm into the ROM (Desktop/Filter, Video/Render/
 * Hourglass): what must hold with no Wimp task running.
 * tests/desktop/filterhourglass (compare.py) compares their behaviour with
 * RISC OS 5.30's.
 *
 * The output of *commands is caught on WrchV.  Filter code is a native
 * entry: a filter is only ever called from a task's Wimp_Poll, and there
 * is none here.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"
#include "rom_filtermgr.h"
#include "rosgd/rom.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define XFILTER_REGISTERPREFILTER 0x62640u
#define XFILTER_DEREGISTERPREFILTER 0x62642u
#define XHOURGLASS_SMASH 0x606C2u
#define XHOURGLASS_START 0x606C3u
#define XHOURGLASS_LEDS 0x606C5u
#define XHOURGLASS_COLOURS 0x606C6u
#define SERVICE_FILTERMANAGERINSTALLED 0x87u

static char out[2048];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* A SWI: its registers in and out; 0, or the error number */
static uint32_t swi(uint32_t number, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, number);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static const char *errmess(const uint32_t r[8])
{
    return ((os_error *)ros_ptr(r[0]))->errmess;
}

/* A command, its output in out: 0 or the error number */
static uint32_t cli(uint32_t line, const char *cmd, char *error, size_t size)
{
    strcpy(ros_ptr(line), cmd);
    outn = 0, out[0] = 0;
    uint32_t r[8] = { line };
    uint32_t e = swi(XOS_CLI, r);
    snprintf(error, size, "%s", e ? errmess(r) : "");
    return e;
}

static const struct ros_module *module(const char *title)
{
    for (const struct ros_module *m = ros_module_first(); m; m = m->next)
        if (strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

/* The pointer's shape number: OS_Byte 106 sets one and returns the old */
static uint32_t pointer(void)
{
    uint32_t r[8] = { 106, 3 };
    swi(XOS_Byte, r);
    uint32_t n = r[1] & 0x7F;
    uint32_t back[8] = { 106, n };
    swi(XOS_Byte, back);
    return n;
}

/* A pointer colour, &BBGGRR00 */
static uint32_t colour(uint32_t c)
{
    uint32_t r[8] = { c, 25 };
    swi(XOS_ReadPalette, r);
    return r[2] & 0xFFFFFF00u;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- the start-up callback: its service call, and background work ---- */

static uint32_t seq, installed_version, work_at, mark_at;

static void work(void *arg, uint32_t info)
{
    (void)arg, (void)info;
    work_at = ++seq;
}

static void watch_service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] == SERVICE_FILTERMANAGERINSTALLED) {
        installed_version = s->r[0];
        ros_post(work, NULL, 0);    /* an interrupt, while the callback runs */
    }
}

static struct ros_module watcher = {
    .title = "FHWatch",
    .help = "FHWatch\t1.00 (26 Sep 2026)",
    .service = watch_service,
};

static void mark(void *arg)
{
    (void)arg;
    mark_at = ++seq;
}

static void filter(struct ros_cpu *s)
{
    s->r[15] = s->r[14];
}

void ros_selftest_filterhourglass(void)
{
    char error[256];
    const struct ros_module *fm = module("FilterManager"), *hg = module("Hourglass");
    /* the compiled FilterManager in the ROM image, or the native Wimp's shim
     * in its place (runtime/rom.c; rosgd.wimp=translated gives the first) */
    int shim = fm == ros_native_filtermgr;
    check(fm && hg && (shim || fm->base == 0xFCB00000u) && hg->base == 0xFCB80000u &&
              ros_module_version(fm) == 0x3000 && ros_module_version(hg) == 0x21900 &&
              fm->swi_chunk == 0x42640 && fm->swi_count == 12 && hg->swi_chunk == 0x406C0 &&
              hg->swi_count == 7,
          "FilterManager 0.30 and Hourglass 2.19 in the ROM, started, their SWI chunks named",
          "FilterManager %s%s, Hourglass %s", fm ? "found" : "missing",
          shim ? " (the native Wimp's)" : "", hg ? "found" : "missing");
    if (!fm || !hg)
        return;

    uint32_t line = ros_addr(ros_rma_alloc(256));
    uint32_t name = ros_addr(ros_rma_alloc(16));
    strcpy(ros_ptr(name), "FHTest");
    uint32_t code = ros_native_entry(filter, "selftest:filter");
    ros_vector_claim_native(WRCHV, wrch, 0);

    /* ---- the Filter Manager: filters for all tasks, *Filters, errors ---- */
    uint32_t r[8] = { name, code, 0x1234, 0 };
    uint32_t reg = swi(XFILTER_REGISTERPREFILTER, r);
    uint32_t listed = cli(line, "Filters", error, sizeof error);
    int shown = strstr(out, "Filters called on entry to Wimp_Poll:") &&
                strstr(out, "FHTest              All tasks") &&
                strstr(out, "Filters called just before a rectangle copy:");
    uint32_t d[8] = { name, code, 0x1234, 0xABCD0000u };
    uint32_t dereg = swi(XFILTER_DEREGISTERPREFILTER, d);
    uint32_t again[8] = { name, code, 0x1234, 0 };
    uint32_t unknown = swi(XFILTER_DEREGISTERPREFILTER, again);
    int unknown_text = unknown && strcmp(errmess(again), "Unknown filter") == 0;
    cli(line, "Filters", error, sizeof error);
    int gone = !strstr(out, "FHTest");
    check(!reg && !listed && shown && !dereg && unknown == 0x605 && unknown_text && gone,
          "Filter_RegisterPreFilter for all tasks: listed by *Filters, deregistered (the task's "
          "top half ignored), then &605 Unknown filter",
          "register &%X, *Filters &%X %d, deregister &%X, again &%X %d, gone %d", reg, listed,
          shown, dereg, unknown, unknown_text, gone);

    uint32_t bad[8] = { 0 };
    uint32_t badswi = swi(ROS_X_BIT | 0x4264C, bad);
    check(badswi == 0x1E6 &&
              strcmp(errmess(bad), "SWI value out of range for module FilterManager") == 0,
          "Filter SWI &4264C: the module's own &1E6, its title in the global message",
          "&%X %s", badswi, badswi ? errmess(bad) : "");

    /* ---- *Help from the modules' Messages files (International_Help) ---- */
    uint32_t h1 = cli(line, "Help Filters", error, sizeof error);
    int help1 = strstr(out, "\r==> Help on keyword Filters") &&
                strstr(out, "*Filters displays all Wimp filters currently active.") &&
                strstr(out, "Syntax: *Filters") && out[0] == 14 && out[outn - 1] == 15;
    uint32_t h2 = cli(line, "Help FilterManager", error, sizeof error);
    int help2 = strstr(out, "Module is: Filter Manager  0.30 (21 Aug 2023)") &&
                strstr(out, "Commands provided:");
    uint32_t h3 = cli(line, "Help Hourglass", error, sizeof error);
    int help3 = strstr(out, "Module is: Hourglass       2.19 (10 Nov 2013)") &&
                strstr(out, "HOn     HOff");
    uint32_t h4 = cli(line, "HOn HOff", error, sizeof error);
    check(!h1 && help1 && !h2 && help2 && !h3 && help3 && h4 == 0xDC &&
              strcmp(error, "Syntax: *HOn") == 0,
          "*Help: a command's help and syntax, and a module's title, from its Messages file; "
          "*HOn's syntax error too",
          "&%X %d, &%X %d, &%X %d, &%X \"%s\"", h1, help1, h2, help2, h3, help3, h4, error);

    /* ---- the native Wimp's *WimpStats: null-event pacing's counts (#138) ---- */
    if (shim) {
        uint32_t ws1 = cli(line, "WimpStats", error, sizeof error);
        int listed1 = strstr(out, "Null events:") && strstr(out, "Nulls      Paced");
        uint32_t ws2 = cli(line, "WimpStats -reset", error, sizeof error);
        check(!ws1 && listed1 && !ws2 && outn == 0,
              "*WimpStats: the native Wimp's null-event pacing and its per-task counts; -reset "
              "quietly",
              "&%X %d, &%X (%u bytes out)", ws1, listed1, ws2, outn);
    }

    ros_vector_release_native(WRCHV, wrch, 0);

    /* ---- the Hourglass: the values it keeps ---- */
    uint32_t c1[8] = { 0x123456, 0x654321 }, c2[8] = { (uint32_t)-1, (uint32_t)-1 };
    swi(XHOURGLASS_COLOURS, c1);
    swi(XHOURGLASS_COLOURS, c2);
    uint32_t c3[8] = { c1[0], c1[1] };
    swi(XHOURGLASS_COLOURS, c3);
    uint32_t l1[8] = { 1, 0 }, l2[8] = { 2, 0xFF }, l3[8] = { 0, 0 };
    swi(XHOURGLASS_LEDS, l1);
    swi(XHOURGLASS_LEDS, l2);
    swi(XHOURGLASS_LEDS, l3);
    check(c1[0] == 0xFFFF00 && c1[1] == 0xFF0000 && c2[0] == 0x123456 && c2[1] == 0x654321 &&
              l1[0] == 0 && l2[0] == 1 && l3[0] == 3,
          "Hourglass_Colours and Hourglass_LEDs: cyan and blue at first; each returns the old",
          "colours &%X &%X, then &%X &%X; LEDs %u %u %u", c1[0], c1[1], c2[0], c2[1], l1[0], l2[0],
          l3[0]);

    /* ---- the Hourglass shown on the ticks, and taken away ---- */
    uint32_t on[8] = { 106, 1 };            /* pointer 1, as the desktop has it */
    swi(XOS_Byte, on);
    uint32_t p0 = pointer(), k1 = colour(1), k3 = colour(3);
    uint32_t start[8] = { 1 };
    swi(XHOURGLASS_START, start);
    uint32_t shape = p0;
    for (int i = 0; i < 50 && shape == p0; i++) {
        ROS_BLOCKING(sleep_ms(10));             /* ticks run meanwhile */
        shape = pointer();
    }
    uint32_t s1 = colour(1), s3 = colour(3);
    uint32_t smash[8] = { 0 };
    swi(XHOURGLASS_SMASH, smash);
    uint32_t p1 = pointer();
    check((shape == 3 || shape == 4) && s1 == 0xFFFF0000u && s3 == 0xFF000000u && p1 == p0 &&
              colour(1) == k1 && colour(3) == k3,
          "Hourglass_Start 1: on TickerV the pointer becomes shape 3 or 4, cyan and blue; "
          "Hourglass_Smash puts pointer 1 and its colours back",
          "shape %u, colours &%08X &%08X; after, pointer %u", shape, s1, s3, p1);
    uint32_t off[8] = { 106, on[1] };       /* the pointer as it was */
    swi(XOS_Byte, off);

    /* ---- the start-up callback, again: background work waits for it ---- */
    ros_module_add(&watcher, "");
    uint32_t info[8] = { 18, ros_addr(strcpy(ros_ptr(line), "FilterManager")) };
    swi(XOS_Module, info);
    seq = installed_version = work_at = mark_at = 0;
    os_error *added = ros_callback_add(FILTERMGR_servicecallback, info[4]);  /* as its Init */
    ros_callback_add_native(mark, NULL);    /* next: when the callback has returned */
    uint32_t t[8] = { 0 };
    swi(XOS_ReadMonotonicTime, t);          /* its way out runs the callbacks */
    swi(XOS_ReadMonotonicTime, t);          /* and this one's the work */
    uint32_t kill[8] = { 4, ros_addr(strcpy(ros_ptr(line), "FHWatch")) };
    swi(XOS_Module, kill);
    check(!added && installed_version == 30 && mark_at == 1 && work_at == 2 &&
              ros_ld32(info[4]) == 0,
          "the Filter Manager's start-up callback: Service_FilterManagerInstalled with its "
          "version; a compiled callback's SWIs are not the outermost, so background work waits",
          "added %d, version %u, callback done at %u, work at %u", !added, installed_version,
          mark_at, work_at);

    ros_rma_free(ros_ptr(name));
    ros_rma_free(ros_ptr(line));
}
