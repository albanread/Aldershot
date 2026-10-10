/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_shellcli.c: ShellCLI, native (modules/shellcli): what must
 * hold before the desktop starts one. The module is there with its SWIs,
 * its command and its greeting. A shell is one at a time, and holds the
 * module while it lasts. Shell_Destroy puts the exit handler back. The
 * shell itself (F12 on the desktop) is tests/desktop/shellcli's box
 * probe.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rom_wimp.h"
#include "selftest.h"
#include "shellcli.h"

#define check ros_check

#define SERVICE_WIMPCLOSEDOWN 0x53u

static uint32_t scratch;

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

static const os_error *err(int v, const uint32_t r[8])
{
    return v ? ros_ptr(r[0]) : NULL;
}

static int is(const os_error *e, uint32_t errnum, const char *text)
{
    return e && e->errnum == errnum && strcmp(e->errmess, text) == 0;
}

/* OS_CLI: 0 or the error */
static const os_error *cli(const char *cmd)
{
    uint32_t r[8] = { str(scratch, cmd) };
    return err(swi(XOS_CLI, r), r);
}

void ros_selftest_shellcli(void)
{
    uint8_t *mem = ros_rma_alloc(512);
    if (!mem) {
        check(0, "ShellCLI: scratch memory", NULL);
        return;
    }
    scratch = ros_addr(mem);

    uint32_t r[8] = { 18, str(scratch, "ShellCLI") };
    int found = !swi(XOS_Module, r);
    struct ros_module *m = NULL;
    for (struct ros_module *x = ros_module_first(); found && x; x = x->next)
        if (x->base == r[3])
            m = x;
    uint32_t c[8] = { 0, str(scratch, "Shell_Create") };
    int cv = swi(XOS_SWINumberFromString, c);
    uint32_t d[8] = { 0, str(scratch, "XShell_Destroy") };
    int dv = swi(XOS_SWINumberFromString, d);
    check(m && ros_module_version(m) == 0x3900 && m->swi_chunk == SHELLCLI_CHUNK && r[4] == 0 &&
          !cv && c[0] == 0x405C0 && !dv && d[0] == 0x605C1,
          "ShellCLI 0.39 is in the ROM, native, no shell yet: its SWIs Shell_Create &405C0 and "
          "Shell_Destroy &405C1", "%s", m ? "" : "not found");

    /* The greeting, from its Messages file, set as it starts */
    uint32_t v[8] = { str(scratch, "CLI$Greeting"), scratch + 64, 255, 0, 0 };
    int gv = swi(XOS_ReadVarVal, v);
    char greeting[256] = "";
    if (!gv) {
        memcpy(greeting, ros_ptr(scratch + 64), v[2]);
        greeting[v[2]] = 0;
    }
    check(!gv && strcmp(greeting, SHELLCLI_GREETING) == 0,
          "CLI$Greeting: \"This is the ShellCLI. Press Return without entering any text to "
          "return to the desktop.\"", "\"%s\"", greeting);

    /* *ShellCLI takes nothing; past the chunk's two SWIs, the module named */
    const os_error *syn = cli("ShellCLI now");
    uint32_t bad[8] = { 0 };
    const os_error *be = err(swi(ROS_X_BIT | 0x405C2, bad), bad);
    uint32_t none[8] = { 0 };
    int destroy_none = swi(XShell_Destroy, none);
    check(is(syn, 0xDC, "Syntax: *ShellCLI") &&
          is(be, 0x1E6, "SWI value out of range for module ShellCLI") && !destroy_none,
          "*ShellCLI with a parameter: its syntax (&DC); SWI &405C2: &1E6, the module named; "
          "Shell_Destroy with no shell: nothing", "%s / %s", syn ? syn->errmess : "no error",
          be ? be->errmess : "no error");

    /* A shell: one at a time, the module held; Shell_Destroy puts the exit
     * handler back.  The shell's handlers are put in the self-test's own
     * environment, so all of it is kept and put back after. */
    struct ros_environment saved;
    ros_env_save(&saved);
    uint32_t exit_before[3], exit_shell[3], error_shell[3], exit_after[3];
    ros_env_read(ROS_ENV_EXIT, &exit_before[0], &exit_before[1], &exit_before[2]);
    uint32_t cr[8] = { 0 };
    int created = !swi(XShell_Create, cr);
    ros_env_read(ROS_ENV_EXIT, &exit_shell[0], &exit_shell[1], &exit_shell[2]);
    ros_env_read(ROS_ENV_ERROR, &error_shell[0], &error_shell[1], &error_shell[2]);
    uint32_t ws[8] = { 18, str(scratch, "ShellCLI") };
    swi(XOS_Module, ws);
    uint32_t block = ws[4];
    uint32_t again[8] = { 0 };
    const os_error *second = err(swi(XShell_Create, again), again);
    int second_ok = is(second, SHELLCLI_ERR_CREATION, "ShellCLI not active");
    const os_error *entered = cli("ShellCLI");
    int entered_ok = is(entered, SHELLCLI_ERR_CREATION, "ShellCLI not active");
    const os_error *kill = cli("RMKill ShellCLI");
    int kill_ok = is(kill, SHELLCLI_ERR_REMOVAL, "ShellCLI task is still active");
    /* Service_WimpCloseDown with R0 > 0 means Wimp_Initialise is about to close
     * the task this domain ran before it, with R2 that task. The shell has
     * none here (0). The service is refused, with the Wimp's own
     * WimpCantKill number (the translated Wimp's error block's, &104). */
    uint32_t sv[8] = { 1, SERVICE_WIMPCLOSEDOWN, 0 };
    swi(XOS_ServiceCall, sv);
    uint32_t cantkill = ros_ld32(WIMP_ErrorBlock_WimpCantKill);
    int refused = sv[1] == SERVICE_WIMPCLOSEDOWN && sv[0] != 1 && sv[0] != 0 &&
                  cantkill == 0x104 && is(ros_ptr(sv[0]), cantkill, "Window Manager is in use");
    uint32_t de[8] = { 0 };
    int destroyed = !swi(XShell_Destroy, de);
    ros_env_read(ROS_ENV_EXIT, &exit_after[0], &exit_after[1], &exit_after[2]);
    uint32_t gone[8] = { 18, str(scratch, "ShellCLI") };
    swi(XOS_Module, gone);
    ros_env_load(&saved);
    check(created && block && exit_shell[0] != exit_before[0] && exit_shell[1] == block &&
          error_shell[1] == block && error_shell[2] == block + 1024,
          "Shell_Create: the shell's block claimed, the exit and error handlers its own, the "
          "error buffer in the block", "block &%X exit &%X/&%X error &%X/&%X/&%X", block,
          exit_shell[0], exit_shell[1], error_shell[0], error_shell[1], error_shell[2]);
    check(second_ok && entered_ok && kill_ok && refused,
          "While a shell exists: Shell_Create and *ShellCLI &900 \"ShellCLI not active\"; "
          "*RMKill &901 \"ShellCLI task is still active\"; Wimp_Initialise closing the shell's "
          "task, the Wimp's WimpCantKill &104 \"Window Manager is in use\"",
          "%s / %s / %s / %s &%X (the Wimp's &%X)",
          second ? second->errmess : "no error", entered ? entered->errmess : "no error",
          kill ? kill->errmess : "no error", refused ? "refused" : "not refused",
          sv[0] > 1 ? ((const os_error *)ros_ptr(sv[0]))->errnum : 0, cantkill);
    /* (Its R12 stays the block's if it was 0. OS_ChangeEnvironment leaves
     * a field given as 0 as it is, and RISC OS's does too.) */
    check(destroyed && gone[4] == 0 && exit_after[0] == exit_before[0] &&
          (exit_after[1] == exit_before[1] || (exit_before[1] == 0 && exit_after[1] == block)),
          "Shell_Destroy: the exit handler's code as it was, the block freed",
          "exit &%X/&%X, before &%X/&%X; private word &%X", exit_after[0], exit_after[1],
          exit_before[0], exit_before[1], gone[4]);

    /* Finalising with no shell is allowed: *RMReInit, and the greeting again */
    const os_error *re = cli("RMReInit ShellCLI");
    uint32_t back[8] = { 18, str(scratch, "ShellCLI") };
    check(!re && !swi(XOS_Module, back),
          "*RMReInit ShellCLI with no shell: it goes and comes back", "%s",
          re ? re->errmess : "");

    ros_rma_free(mem);
}
