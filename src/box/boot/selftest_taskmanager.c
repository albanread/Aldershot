/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_taskmanager.c: the Task Manager (Desktop/Switcher), compiled
 * from ObjAsm into the ROM: what holds before the desktop starts, as RISC OS
 * 5.30's does (tests/desktop/taskmanager holds the two side by side).
 *
 * Until *Desktop the module has no workspace, and its SWIs say it is not
 * active. Service_StartWimp, which *Desktop issues, gives it its workspace,
 * whose size is that of its templates found through the Wimp. The service
 * also names the command that starts it as a task. *RMReInit frees the
 * workspace again. Its errors and its commands' help are looked up in its
 * Messages.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define XTASKMANAGER_TASKNAMEFROMHANDLE 0x62680u
#define XTASKMANAGER_ENUMERATETASKS 0x62681u
#define SERVICE_STARTWIMP 0x49u
#define ERR_UNKNOWN_TASK 0x81F402u
#define ERR_USE_DESKTOP 0x81F403u
#define ERR_NOT_ACTIVE 0x81F404u

static uint32_t text;               /* arena: names, commands, a buffer */
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

static int swi7(uint32_t n, uint32_t r[7])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 7 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 7 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[7])
{
    return ((os_error *)ros_ptr(r[0]))->errnum;
}

static const char *errmess(const uint32_t r[7])
{
    return ((os_error *)ros_ptr(r[0]))->errmess;
}

/* OS_CLI: the error number or 0, what it printed in out, the error's text
 * in mess. */
static uint32_t cli(const char *cmd, char *mess, size_t n)
{
    strcpy(ros_ptr(text), cmd);
    outn = 0, out[0] = 0;
    uint32_t r[7] = { text, 0, 0, 0, 0, 0, 0 };
    int v = swi7(XOS_CLI, r);
    if (mess && n)
        strncpy(mess, v ? errmess(r) : "", n - 1), mess[n - 1] = 0;
    return v ? errnum(r) : 0;
}

/* A Task Manager SWI with R0-R2: the error number or 0; R0 back. */
static uint32_t tm(uint32_t swi, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t *r0_out)
{
    uint32_t r[7] = { r0, r1, r2, 0, 0, 0, 0 };
    int v = swi7(swi, r);
    if (r0_out)
        *r0_out = r[0];
    return v ? errnum(r) : 0;
}

void ros_selftest_taskmanager(void)
{
    if (!text)
        text = ros_addr(ros_rma_alloc(512));

    /* ---- in the ROM ---- */
    strcpy(ros_ptr(text), "TaskManager");
    uint32_t r[7] = { 18, text, 0, 0, 0, 0, 0 };
    int found = !swi7(XOS_Module, r);
    uint32_t base = found ? r[3] : 0;
    const char *help = found ? (const char *)ros_ptr(base + ros_ld32(base + 0x14)) : "";
    strcpy(ros_ptr(text), "TaskManager_StartTask");
    uint32_t n[7] = { 0, text, 0, 0, 0, 0, 0 };
    int named = !swi7(XOS_SWINumberFromString, n) && n[0] == 0x42683;
    check(found && strcmp(help, "Task Manager\t1.54 (10 Jul 2021)") == 0 &&
          ros_ld32(base + 0x1C) == 0x42680 && named,
          "TaskManager: in the ROM, 1.54, its SWIs at &42680 by name",
          "found %d, help \"%s\", SWI named %d", found, help, named);

    /* ---- before the desktop ---- */
    /* Service_StartWimp, which the Pinboard's self-test issues as the
     * Desktop module does, gives the Task Manager its workspace, and it
     * keeps it until its task starts: *RMReInit starts it again as the ROM
     * did, dormant. */
    char mess[128];
    uint32_t e0 = cli("RMReInit TaskManager", mess, sizeof mess);
    uint32_t e1 = tm(XTASKMANAGER_ENUMERATETASKS, 0, text + 256, 64, NULL);
    uint32_t e2 = tm(XTASKMANAGER_TASKNAMEFROMHANDLE, 0, 0, 0, NULL);
    uint32_t e3 = cli("Desktop_TaskManager", mess, sizeof mess);
    check(e0 == 0 && e1 == ERR_NOT_ACTIVE && e2 == ERR_NOT_ACTIVE && e3 == ERR_USE_DESKTOP &&
          strcmp(mess, "Use *Desktop to start TaskManager") == 0,
          "TaskManager: before *Desktop its SWIs are not active, &81F404; *Desktop_TaskManager "
          "says to use *Desktop, looked up in its Messages",
          "&%X &%X &%X &%X \"%s\"", e0, e1, e2, e3, mess);

    /* ---- its commands' help, from its Messages ---- */
    ros_vector_claim_native(WRCHV, wrch, 0);
    uint32_t h1 = cli("Help Desktop_TaskManager", NULL, 0);
    int help_ok = h1 == 0 &&
                  strstr(out, "\r==> Help on keyword Desktop_TaskManager\n\r"
                              "The Task Manager module provides task management under the Desktop.\n\r"
                              "Do not use *Desktop_TaskManager, use *Desktop instead.\n\r"
                              "Syntax: *Desktop_TaskManager\n\r") != NULL;
    uint32_t h2 = cli("Help TaskManager", NULL, 0);
    int module_ok = h2 == 0 &&
                    strstr(out, "Module is: Task Manager    1.54 (10 Jul 2021)\n\r\n\r"
                                "Commands provided:\n\rDesktop_TaskManager     StartDesktopTask\n\r")
                    != NULL;
    ros_vector_release_native(WRCHV, wrch, 0);
    uint32_t s1 = cli("StartDesktopTask", mess, sizeof mess);
    check(help_ok && module_ok && s1 == 0xDC && strcmp(mess, "Syntax: *StartDesktopTask <*command>") == 0,
          "TaskManager: *Help on its commands, and their syntax errors, from its Messages; "
          "*Help TaskManager, the module and its commands",
          "help %d, module %d, syntax &%X \"%s\"", help_ok, module_ok, s1, mess);

    /* ---- Service_StartWimp: its workspace, and no tasks ---- */
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_STARTWIMP;
    ros_service_call(&s);
    int claimed = s.r[1] == 0 && strcmp(ros_ptr(s.r[0]), "Desktop_TaskManager") == 0;
    uint32_t more = 0;
    uint32_t w1 = tm(XTASKMANAGER_ENUMERATETASKS, 0, text + 256, 64, &more);
    uint32_t w2 = tm(XTASKMANAGER_TASKNAMEFROMHANDLE, 0, 0, 0, NULL);
    uint32_t w3 = cli("RMReInit TaskManager", mess, sizeof mess);
    uint32_t w4 = tm(XTASKMANAGER_ENUMERATETASKS, 0, text + 256, 64, NULL);
    check(claimed && w1 == 0 && more == 0xFFFFFFFFu && w2 == ERR_UNKNOWN_TASK && w3 == 0 &&
          w4 == ERR_NOT_ACTIVE,
          "TaskManager: Service_StartWimp claims its workspace, its templates sized by the Wimp, "
          "and names *Desktop_TaskManager; no tasks known; *RMReInit frees it",
          "claimed %d, enumerate &%X R0 &%X, name &%X, reinit &%X \"%s\", then &%X",
          claimed, w1, more, w2, w3, mess, w4);
}
