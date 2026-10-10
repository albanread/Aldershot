/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_pinboard.c: the Pinboard (Desktop/Pinboard), compiled from
 * ObjAsm into the ROM at &FC800000, as a module before the Wimp starts it:
 * its header, its workspace, its resources, the services it takes from the
 * Wimp, and the *commands that answer without a task. What it does as a
 * task waits for Wimp tasks (tests/desktop/pinboard, task.bas).
 *
 * OS_CLI's International_Help is checked too, which the Pinboard's commands
 * found. A command's syntax and help are tokens in the module's Messages.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define SERVICE_STARTWIMP   0x49u
#define SERVICE_STARTEDWIMP 0x4Au
#define PINBOARD_BASE 0xFC800000u

static uint32_t text;               /* arena scratch: names, commands, results */
static char out[2048];              /* what a command printed */
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

static int swi(uint32_t n, uint32_t r[6])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 6 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 6 * sizeof r[0]);
    return (int)s.v;
}

static uint32_t str(uint32_t at, const char *s)
{
    strcpy(ros_ptr(at), s);
    return at;
}

/* OS_CLI: its error or NULL, what it printed in out. */
static const os_error *cli(const char *cmd)
{
    outn = 0, out[0] = 0;
    uint32_t r[6] = { str(text, cmd), 0, 0, 0, 0, 0 };
    ros_vector_claim_native(WRCHV, wrch, 0);
    int v = swi(XOS_CLI, r);
    ros_vector_release_native(WRCHV, wrch, 0);
    return v ? ros_ptr(r[0]) : NULL;
}

/* OS_Module 18: its base and workspace; 0 if it is not there. */
static uint32_t lookup(uint32_t *ws)
{
    uint32_t r[6] = { 18, str(text, "Pinboard"), 0, 0, 0, 0 };
    if (swi(XOS_Module, r))
        return 0;
    *ws = r[4];
    return r[3];
}

static uint32_t service(uint32_t number, uint32_t *r0)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = *r0, s.r[1] = number;
    ros_service_call(&s);
    *r0 = s.r[0];
    return s.r[1];
}

void ros_selftest_pinboard(void)
{
    text = ros_addr(ros_rma_alloc(512));
    uint32_t ws = 0, base = lookup(&ws);
    struct ros_module *m = NULL;
    for (struct ros_module *x = ros_module_first(); x; x = x->next)
        if (x->base == base && base)
            m = x;
    const char *help = base ? ros_ptr(base + ros_ld32(base + 0x14)) : "";
    check(base == PINBOARD_BASE && m && strcmp(m->title, "Pinboard") == 0 &&
              strcmp(help, "Pinboard\t1.07 (24 Jun 2023)") == 0 &&
              ros_module_version(m) == 0x00010700u && m->swi_chunk == 0,
          "Pinboard: started from its ROM header at &FC800000 -- 1.07, the 5.31 sources', no SWIs",
          "base &%08X, \"%s\"", base, help);
    if (!m)
        goto done;

    check(ws >= ROS_RMA_BASE && ws < ROS_RMA_BASE + ROS_RMA_SIZE && ros_ld32(ws) == 0,
          "Pinboard: Init claimed its workspace; no task (handle 0) until the Wimp starts one",
          "workspace &%08X", ws);

    uint32_t v[6] = { str(text, "Pinboard$Path"), text + 64, 200, 0, 3, 0 };
    int path = !swi(XOS_ReadVarVal, v) && v[2] == 31 &&
               memcmp(ros_ptr(text + 64), "Resources:$.Resources.Pinboard.", 31) == 0;
    uint32_t o[6] = { text + 256, str(text, "Pinboard:Messages"), 0, 0, 0, 0 };
    int opened = !swi(XMessageTrans_OpenFile, o);
    uint32_t l[6] = { text + 256, str(text + 32, "TaskID"), text + 64, 64, 0, 0 };
    int named = opened && !swi(XMessageTrans_Lookup, l) && strcmp(ros_ptr(text + 64), "Pinboard") == 0;
    if (opened) {
        uint32_t c[6] = { text + 256, 0, 0, 0, 0, 0 };
        swi(XMessageTrans_CloseFile, c);
    }
    uint32_t f[6] = { 17, str(text, "Pinboard:Templates"), 0, 0, 0, 0 };
    int templates = !swi(XOS_File, f) && f[0] == 1 && (f[2] & 0xFFFFFF00u) == 0xFFFFEC00u;
    check(path && named && templates,
          "Pinboard: Pinboard$Path is its resources; its Messages name the task, its Templates are there",
          "path %d, TaskID %d, Templates %d", path, named, templates);

    /* The Wimp's start-up: the command that starts the task is offered at
     * Service_StartWimp, with the handle -1 while it is starting. It is taken
     * back at Service_StartedWimp when no task started. The Desktop
     * module issues the service until no one claims it, and a task counts as
     * a claim. Another module in the ROM (the Filer) may claim it first. */
    uint32_t r0 = 0, claimed = 0;
    int offered = 0;
    for (int i = 0; i < 16 && !offered && claimed == 0; i++) {
        r0 = 0;
        claimed = service(SERVICE_STARTWIMP, &r0);
        offered = claimed == 0 && r0 && strcmp(ros_ptr(r0), "Desktop_Pinboard") == 0 &&
                  ros_ld32(ws) == 0xFFFFFFFFu;
    }
    r0 = 0;
    service(SERVICE_STARTEDWIMP, &r0);
    check(offered && ros_ld32(ws) == 0,
          "Pinboard: Service_StartWimp claimed with *Desktop_Pinboard; Service_StartedWimp resets it",
          "R1 %u, handle &%X", claimed, ros_ld32(ws));

    const os_error *e = cli("Desktop_Pinboard");
    int desk = e && e->errnum == 0 && strcmp(e->errmess, "Use *Desktop to start Pinboard") == 0;
    /* Its own Messages are opened by the task: without one, its errors are
     * looked up in the Global messages, which lack them, as on RISC OS. */
    e = cli("BackDrop -Centre -Tile");
    int opts = e && strcmp(e->errmess, "Message token BadOpts not found") == 0;
    check(desk && opts,
          "Pinboard: *Desktop_Pinboard outside the desktop refused; *BackDrop's BadOpts, with no task to open its Messages, not found",
          "%s", e ? e->errmess : "no error");

    /* OS_CLI's International_Help: the syntax and help are the module's
     * tokens, SPINPIN and HPINPIN, looked up in Pinboard:Messages. */
    e = cli("Pin");
    int syntax = e && e->errnum == 0xDC && strcmp(e->errmess, "Syntax: *Pin <pathname> <x> <y>") == 0;
    e = cli("Help Pin");
    int helped = !e && strstr(out, "*Pin adds a file, application or directory to the desktop pinboard.") &&
                 strstr(out, "Syntax: *Pin <pathname> <x> <y>") && !strstr(out, "HPINPIN");
    /* HPINOPT goes on over lines to its NUL, its breaks 27 1 */
    e = cli("Help PinboardOptions");
    int lines = !e && strstr(out, "used by Pinboard.\n\rSwitches:\n\r-Grid") &&
                strstr(out, "stacked horizontally.\n\rSyntax: *PinboardOptions <switches>");
    check(syntax && helped && lines,
          "OS_CLI: International_Help -- *Pin's syntax error, *Help Pin and *Help PinboardOptions from the module's Messages",
          "\"%s\"", out);

    e = cli("RMReInit Pinboard");
    uint32_t ws2 = 0;
    int again = !e && lookup(&ws2) == PINBOARD_BASE && ws2 && ros_ld32(ws2) == 0;
    check(again, "Pinboard: *RMReInit -- it dies with no task to close, and starts again",
          "%s", e ? e->errmess : "");

done:
    ros_rma_free(ros_ptr(text));
}
