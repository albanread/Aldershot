/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_filer.c: the Filer and FilerSWIs, compiled from ObjAsm into
 * the ROM (Desktop/Filer, Desktop/FilerSWIs).  It tests what holds before
 * there is a desktop to start the Filer in.  That is the modules being
 * there, the Filer's small workspace and its *commands, its service calls,
 * its help, and FilerSWIs' selection.  tests/desktop/filer checks the
 * desktop's side and the rest, against RISC OS 5.30.
 *
 * The Filer's small workspace (s.WkspEtc): +0 its task handle (0 dormant,
 * -1 asked to start), +8 its viewers, +12 its queued requests, +72 the
 * Filer_Action options, +74 the layout, +76 the visibility, +80 the
 * double-click hold.  The options, layout and visibility are bytes.  The
 * first two are in fields of two bytes, and nothing sets the second byte.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define NOWT 0x40000001u                    /* the Filer's empty list */
#define XFILERACTION_SENDSELECTEDFILE 0x60F81u

static uint32_t line;                       /* arena: a command, or a string */
static const os_error *last;
static char out[4096];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI: the error number or 0; what it printed in out. */
static uint32_t cli(const char *cmd)
{
    strcpy(ros_ptr(line), cmd);
    outn = 0, out[0] = 0;
    ros_vector_claim_native(WRCHV, wrch, 0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line;
    ros_swi(&s, XOS_CLI);
    ros_vector_release_native(WRCHV, wrch, 0);
    last = s.v ? ros_ptr(s.r[0]) : NULL;
    return last ? last->errnum : 0;
}

static const char *last_error(void)
{
    return last ? last->errmess : "";
}

static struct ros_module *module(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (m->title && strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

/* The workspace a module's private word points at, or 0 */
static uint32_t workspace(const char *title)
{
    struct ros_module *m = module(title);
    return m && m->private_word ? ros_ld32(m->private_word) : 0;
}

/* A service call to the Filer alone, through its service entry as
 * OS_ServiceCall makes it: another module that claims the call first,
 * Service_StartWimp say, is not asked. */
static void filer_service(struct ros_cpu *s)
{
    struct ros_module *m = module("Filer");
    uint32_t depth = ros_call_depth;
    ros_call_depth = depth + 1;
    s->r[12] = m->private_word;
    s->r[14] = ROS_RETURN_TO_NATIVE;
    ros_call(s, m->service_addr);
    ros_call_depth = depth;
}

void ros_selftest_filer(void)
{
    line = ros_addr(ros_rma_alloc(512));
    struct ros_module *filer = module("Filer"), *swis = module("FilerSWIs");
    check(filer && filer->base == 0xFC700000u && workspace("Filer") && swis &&
              swis->base == 0xFC780000u && workspace("FilerSWIs") && swis->swi_chunk == 0x40F80u,
          "The Filer and FilerSWIs start from the ROM, at &FC700000 and &FC780000, with "
          "workspace; FilerSWIs' chunk &40F80", NULL);
    if (!filer || !swis)
        return;

    uint32_t w = workspace("Filer");
    strcpy(ros_ptr(line), "Filer$Path");
    uint32_t value = line + 64;
    struct ros_cpu v;
    ros_cpu_enter(&v);
    v.r[0] = line, v.r[1] = value, v.r[2] = 128, v.r[4] = 3;
    ros_swi(&v, XOS_ReadVarVal);
    int path = !v.v && v.r[2] == 28 && memcmp(ros_ptr(value), "Resources:$.Resources.Filer.", 28) == 0;
    check(ros_ld32(w) == 0 && ros_ld8(w + 72) == 9 && ros_ld32(w + 8) == NOWT &&
              ros_ld32(w + 12) == NOWT && path,
          "The Filer, initialised: no task, Filer_Action's options Verbose and Newer, no "
          "viewers or requests; Filer$Path its resources", "task &%X, options &%X",
          ros_ld32(w), ros_ld8(w + 72));

    /* Its finalisation returns through the register it kept its link in
     * (FreeWorkspace's MOV pc, r5).  The compiler leaves a resume point for
     * that. */
    check(cli("Filer_Options -ConfirmAll") == 0 && cli("RMReInit Filer") == 0 &&
              (w = workspace("Filer")) != 0 && ros_ld8(w + 72) == 9,
          "*RMReInit Filer -- killed and started again, its options back to the default",
          "%s", last_error());

    cli("Filer_OpenDir");
    int s1 = last && last->errnum == 0xDC &&
             strcmp(last_error(), "Syntax: *Filer_OpenDir <full dirname> [<x> <y> "
                                  "[<width> <height>]] [<switches>]") == 0;
    cli("Filer_Options -Verbose -Verbose");
    int s2 = last && last->errnum == 0xAC2;
    check(s1 && s2,
          "*Filer_OpenDir with no directory: the syntax from Filer:Messages (International "
          "help); the Filer's own syntax errors want the desktop's messages", "%s", last_error());

    int d1 = cli("Filer_OpenDir $") == 0 && strcmp(last_error(), "Use *Desktop to start Filer") == 0;
    int d2 = cli("Desktop_Filer") == 0 && strcmp(last_error(), "Use *Desktop to start Filer") == 0;
    check(last && d1 && d2 && ros_ld32(w + 12) == NOWT,
          "*Filer_OpenDir, *Desktop_Filer outside the desktop: \"Use *Desktop to start Filer\", "
          "nothing queued", "%s", last_error());

    int o1 = cli("Filer_Options -ConfirmAll -Verbose -Force -Newer -Faster") == 0 &&
             ros_ld8(w + 72) == 0x4F;
    int o2 = cli("Filer_Layout -SmallIcons -SortByDate -ReverseSort") == 0 &&
             ros_ld8(w + 74) == 0x4D;
    int o3 = cli("Filer_DClickHold 25") == 0 && ros_ld32(w + 80) == 25;
    int o4 = cli("Filer_Visibility -ExcludeOS") == 0 && ros_ld8(w + 76) == 1;
    check(o1 && o2 && o3 && o4,
          "*Filer_Options, _Layout, _DClickHold, _Visibility set the Filer's options",
          "&%X &%X %u %u", ros_ld8(w + 72), ros_ld8(w + 74), ros_ld32(w + 80), ros_ld8(w + 76));
    cli("RMReInit Filer");
    w = workspace("Filer");

    /* Asked to start, it gives the command that starts it and waits.  Once
     * the desktop has started, it goes back to dormant (Filer 2.50). */
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x49;                                  /* Service_StartWimp */
    filer_service(&s);
    int w1 = s.r[1] == 0 && s.r[0] && strcmp(ros_ptr(s.r[0]), "Desktop_Filer") == 0 &&
             ros_ld32(w) == 0xFFFFFFFFu;
    ros_cpu_enter(&s);
    s.r[1] = 0x49;
    filer_service(&s);
    int w2 = s.r[1] == 0x49;
    ros_cpu_enter(&s);
    s.r[1] = 0x4A;                                  /* Service_StartedWimp */
    filer_service(&s);
    check(w1 && w2 && s.r[1] == 0x4A && ros_ld32(w) == 0,
          "Service_StartWimp: the Filer claims it with *Desktop_Filer and waits; asked "
          "again, passes; Service_StartedWimp: dormant again", "task &%X", ros_ld32(w));

    int h1 = cli("Help Filer") == 0 && strstr(out, "==> Help on keyword Filer") &&
             strstr(out, "Module is: Filer") && strstr(out, "Commands provided:") &&
             strstr(out, "Filer_OpenDir   Filer_CloseDir");
    int h2 = cli("Help Filer_Run") == 0 &&
             strstr(out, "*Filer_Run is equivalent of double clicking on an object") &&
             strstr(out, "Syntax: *Filer_Run [-Shift|-NoShift] <file>|<application>");
    int h3 = cli("Help Filer_OpenDir") == 0 &&
             strstr(out, "open a directory viewer.\n\rOptions are taken");
    check(h1 && h2 && h3,
          "*Help Filer: the module and its commands; *Help Filer_Run: its help and syntax from "
          "Filer:Messages, the dictionary's 27 1 a new line", "\"%.200s\"", out);

    /* FilerSWIs keeps the selection in its block until it would overflow */
    cli("RMReInit FilerSWIs");
    uint32_t sel = workspace("FilerSWIs");
    static const char *names[] = { "one", "two", "th ree", "a\tb" };
    int f1 = sel && ros_ld8(sel) == 0;
    for (unsigned i = 0; i < 4 && f1; i++) {
        strcpy(ros_ptr(line), names[i]);
        ros_cpu_enter(&s);
        s.r[0] = 0xBAD, s.r[1] = line;
        ros_swi(&s, XFILERACTION_SENDSELECTEDFILE);
        f1 = !s.v && s.r[0] == 0xBAD;
    }
    int f2 = f1 && strcmp(ros_ptr(sel), "one two th ree a") == 0;
    ros_cpu_enter(&s);
    ros_swi(&s, XFILERACTION_SENDSELECTEDFILE + 2);
    int f3 = s.v && ((os_error *)ros_ptr(s.r[0]))->errnum == 0x110;
    cli("RMReInit FilerSWIs");
    sel = workspace("FilerSWIs");
    check(f2 && f3 && sel && ros_ld8(sel) == 0,
          "FilerAction_SendSelectedFile: the names a space apart, each to a control "
          "character; a SWI past the three refused", "\"%s\"", sel ? (char *)ros_ptr(sel) : "");
    ros_rma_free(ros_ptr(line));
}
