/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_taskwindow.c: TaskWindow, native (modules/taskwindow): what
 * must hold before any task window runs. The module is there with its
 * SWI, commands and file types. Outside a task window TaskWindow_TaskInfo
 * says so. The old form's handles and the control-character filter are as
 * RISC OS's module has them. A task window itself needs the Wimp
 * running tasks, and tests/desktop/taskwindow holds that against RISC OS 5.30.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"
#include "selftest.h"
#include "taskwindow.h"

#define check ros_check

#define WRCHV 0x03u

static uint32_t scratch;
static char out[256];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

static uint32_t str(uint32_t at, const char *s)
{
    strcpy(ros_ptr(at), s);
    return at;
}

/* OS_CLI, its output caught: 0 or the error, its text in *text */
static uint32_t cli(const char *cmd, const char **text)
{
    outn = 0, out[0] = 0;
    uint32_t r[8] = { str(scratch, cmd) };
    ros_vector_claim_native(WRCHV, wrch, 0);
    int v = swi(XOS_CLI, r);
    ros_vector_release_native(WRCHV, wrch, 0);
    *text = v ? ((os_error *)ros_ptr(r[0]))->errmess : "";
    return v ? ((os_error *)ros_ptr(r[0]))->errnum : 0;
}

/* A variable's value, or "" */
static const char *var(const char *name)
{
    static char value[128];
    uint32_t r[8] = { str(scratch, name), scratch + 128, 127, 0, 0 };
    if (swi(XOS_ReadVarVal, r))
        return "";
    memcpy(value, ros_ptr(scratch + 128), r[2]);
    value[r[2]] = 0;
    return value;
}

/* What the filter lets through of a VDU stream: how many bytes */
static size_t filtered(const uint8_t *in, size_t n, int ctrl, char *got)
{
    uint8_t counter = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (taskwindow_passes(&counter, ctrl, in[i]))
            got[k++] = (char)in[i];
    got[k] = 0;
    return k;
}

void ros_selftest_taskwindow(void)
{
    uint8_t *mem = ros_rma_alloc(512);
    if (!mem) {
        check(0, "TaskWindow: scratch memory", NULL);
        return;
    }
    scratch = ros_addr(mem);

    uint32_t r[8] = { 18, str(scratch, "TaskWindow") };
    int found = !swi(XOS_Module, r);
    struct ros_module *m = NULL;
    for (struct ros_module *x = ros_module_first(); found && x; x = x->next)
        if (x->base == r[3])
            m = x;
    check(m && ros_module_version(m) == 0x8500 && m->swi_chunk == 0x43380 && r[4] != 0,
          "TaskWindow 0.85 is in the ROM, native, its SWI chunk &43380, its workspace "
          "claimed", NULL);

    /* The file types and their run actions, set as it starts */
    check(strcmp(var("Alias$@RunType_FD7"),
                 "TaskWindow \"Obey %*0\" -name \"Task Obey\" -quit") == 0 &&
          strcmp(var("Alias$@RunType_FD6"),
                 "TaskWindow \"Exec %*0\" -name \"Task Exec\" -display") == 0 &&
          strcmp(var("File$Type_FD7"), "TaskObey") == 0 &&
          strcmp(var("File$Type_FD6"), "TaskExec") == 0,
          "TaskObey (&FD7) and TaskExec (&FD6): their names, and *TaskWindow to run them", NULL);

    /* Not in a task window: TaskWindow_TaskInfo says so; the chunk's end */
    uint32_t info[8] = { 0 };
    int v = swi(ROS_X_BIT | 0x43380, info);
    uint32_t bad[8] = { 0 };
    int bv = swi(ROS_X_BIT | 0x43381, bad);
    const os_error *be = bv ? ros_ptr(bad[0]) : NULL;
    check(!v && info[0] == 0 && be && be->errnum == 0x1E6 &&
          strcmp(be->errmess, "SWI value out of range for module TaskWindow") == 0,
          "TaskWindow_TaskInfo outside a task window: 0; SWI &43381 -- &1E6, the module named",
          "R0 &%X", info[0]);

    const char *text;
    uint32_t e1 = cli("TaskWindow", &text);
    int syntax1 = e1 == 0xDC &&
                  strncmp(text, "Syntax: *TaskWindow <command> [[-wimpslot]", 42) == 0;
    uint32_t e2 = cli("ShellCLI_Task 1", &text);
    int syntax2 = e2 == 0xDC && strcmp(text, "Syntax: *ShellCLI_Task XXXXXXXX XXXXXXXX") == 0;
    uint32_t e3 = cli("ShellCLI_TaskQuit", &text);
    check(syntax1 && syntax2 && e3 == 0 && outn == 0,
          "*TaskWindow and *ShellCLI_Task, too few parameters: their syntax (&DC); "
          "*ShellCLI_TaskQuit outside a task window does nothing", "&%X &%X &%X", e1, e2, e3);

    /* The old form: two handles, eight hex digits and a space each (as
     * !Edit writes them); without the last space it is a command */
    uint32_t task = 0, txt = 0, t2, x2;
    int a = taskwindow_parent_handles("12E039F8 00005001 ", &task, &txt);
    int b = taskwindow_parent_handles("  0000abcd 00000001 Echo", &t2, &x2);
    int c = taskwindow_parent_handles("12E039F8 00005001", &t2, &x2);
    int d = taskwindow_parent_handles("12E039F8 0000500", &t2, &x2);
    int e = taskwindow_parent_handles("\"Echo hi\" -quit", &t2, &x2);
    check(a && task == 0x12E039F8u && txt == 0x5001 && b && c == 0 && d == 0 && e == 0,
          "*ShellCLI_Task's handles: \"<task> <txt> \"; without the space after the second, "
          "or not hex, the *TaskWindow form", NULL);

    /* The control filter, on the farm's stream (tests/desktop/taskwindow,
     * probe ctrl): each control character dropped with its sequence's
     * bytes, LF and 127 kept; -ctrl lets all of it through */
    static const uint8_t stream[] = {
        65, 7, 66, 17, 1, 67, 31, 1, 2, 68, 13, 10, 69, 23, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        70, 9, 71, 8, 72, 127, 73, 0, 74, 10,
    };
    char got[64], all[64];
    size_t n = filtered(stream, sizeof stream, 0, got);
    size_t n_all = filtered(stream, sizeof stream, 1, all);
    check(n == 13 && strcmp(got, "ABCD\nEFGH\x7FIJ\n") == 0 && n_all == sizeof stream &&
          memcmp(all, stream, sizeof stream) == 0,
          "Without -ctrl: \"ABCD<10>EFGH<127>IJ<10>\", as RISC OS 5.30 sent it; with -ctrl, "
          "every byte, the sequences' parameters too", "%zu, %zu", n, n_all);

    /* Finalising with no task window running is allowed: *RMReInit */
    uint32_t e4 = cli("RMReInit TaskWindow", &text);
    uint32_t again[8] = { 18, str(scratch, "TaskWindow") };
    check(e4 == 0 && !swi(XOS_Module, again) && again[4] != 0,
          "*RMReInit TaskWindow with no task window: it goes and comes back", "&%X %s", e4, text);

    ros_rma_free(mem);
}
