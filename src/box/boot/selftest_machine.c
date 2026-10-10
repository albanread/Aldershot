/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_machine.c: Machine, native (modules/machine): what a box
 * managed from another machine is asked.
 *
 * *Reboot and *PowerOff are not run, for obvious reasons. The test holds
 * them to being there, with their syntax, which is what a mistyped command
 * meets. The rest is held to what must be true of any machine. *MachineInfo
 * says what the kernel is and keeps to its own shape. *KernelLog either
 * prints the kernel's messages or says why it cannot, and does not fail
 * silently.
 */
#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check
#define WRCHV 0x03u

static uint32_t line;
static const os_error *last;
static char out[4096];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1 && s->r[0] != 13)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

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

void ros_selftest_machine(void)
{
    line = ros_addr(ros_rma_alloc(512));

    uint32_t e = cli("MachineInfo");
    check(!e && strstr(out, "Kernel") && strstr(out, "Screen") && strstr(out, "Running"),
          "*MachineInfo -- the kernel, how long it has run, and the screen",
          "error &%X, \"%.200s\"", e, out);

    e = cli("KernelLog");
    int said = strlen(out) > 4;
    check(said || e, "*KernelLog -- the kernel's messages, or why there are none",
          "error &%X, \"%.120s\"", e, out);

    e = cli("KernelLog thiswordisnotinanymessage");
    check(!e ? strstr(out, "No kernel message") != NULL : 1,
          "*KernelLog <text> -- a word no message has says so", "error &%X, \"%.120s\"", e, out);

    e = cli("Help Reboot");
    int reboot_known = strstr(out, "Reboot") != NULL;
    uint32_t e2 = cli("Help PowerOff");
    check(!e && !e2 && reboot_known && strstr(out, "PowerOff") != NULL,
          "*Reboot and *PowerOff -- there, with their syntax, for a box with no keyboard",
          "errors &%X &%X, \"%.120s\"", e, e2, out);
}
