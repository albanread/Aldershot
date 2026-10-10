/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_deskmeter.c: the runtime's busyness counters (OS_ReadSysInfo
 * 100, runtime/meter.c) and the Desk Meter module that graphs them. */
#include <stdio.h>
#include <string.h>

#include "deskmeter.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check

#define SERVICE_STARTWIMP 0x49u
#define SERVICE_RESET     0x27u

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

static struct ros_module *module(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (m->title && strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

void ros_selftest_deskmeter(void)
{
    uint32_t *block = ros_rma_alloc(64);
    check(block != NULL, "a counter block is claimed", NULL);

    /* ---- OS_ReadSysInfo 100: the three counters ---------------------- */
    {
        uint32_t r[8] = { 100, ros_addr(block) };
        check(!swi(XOS_ReadSysInfo, r), "OS_ReadSysInfo 100 fills a block", NULL);
        uint64_t now1 = (uint64_t)ros_ld32(ros_addr(block) + 4) << 32 |
                        ros_ld32(ros_addr(block));
        uint64_t swi1 = (uint64_t)ros_ld32(ros_addr(block) + 12) << 32 |
                        ros_ld32(ros_addr(block) + 8);
        uint64_t idle1 = (uint64_t)ros_ld32(ros_addr(block) + 20) << 32 |
                         ros_ld32(ros_addr(block) + 16);
        check(now1 > 0, "now_ns counts from the start", "%llu",
              (unsigned long long)now1);
        check(swi1 > 0, "swi_ns counts the SWIs run already", "%llu",
              (unsigned long long)swi1);

        /* some work, then the counters must have moved */
        for (int i = 0; i < 200; i++) {
            uint32_t w[8] = { 0 };
            swi(XOS_ReadMonotonicTime, w);
        }
        r[0] = 100, r[1] = ros_addr(block);
        check(!swi(XOS_ReadSysInfo, r), "the counters read again", NULL);
        uint64_t swi2 = (uint64_t)ros_ld32(ros_addr(block) + 12) << 32 |
                        ros_ld32(ros_addr(block) + 8);
        check(swi2 > swi1, "swi_ns is monotonic under work", "%llu -> %llu",
              (unsigned long long)swi1, (unsigned long long)swi2);
        (void)idle1;

        /* an idle wait (Portable_Idle's, the box's own) must add idle_ns */
        uint64_t idle2 = (uint64_t)ros_ld32(ros_addr(block) + 20) << 32 |
                         ros_ld32(ros_addr(block) + 16);
        uint32_t p[8] = { 20 };
        swi(XPortable_Idle, p);                 /* a 20ms idle wait */
        r[0] = 100, r[1] = ros_addr(block);
        check(!swi(XOS_ReadSysInfo, r), "the counters read a third time", NULL);
        uint64_t idle3 = (uint64_t)ros_ld32(ros_addr(block) + 20) << 32 |
                         ros_ld32(ros_addr(block) + 16);
        check(idle3 > idle2, "idle_ns counts Portable_Idle's wait",
              "%llu -> %llu", (unsigned long long)idle2, (unsigned long long)idle3);

        /* without a block the call is refused (its one error) */
        r[0] = 100, r[1] = 0;
        check(swi(XOS_ReadSysInfo, r), "without a block it is refused", NULL);
    }

    /* ---- the module --------------------------------------------------- */
    struct ros_module *m = module("DeskMeter");
    check(m != NULL, "DeskMeter is in the chain", NULL);
    check(m && m->commands && strcmp(m->commands[0].name, "DeskMeter") == 0,
          "*DeskMeter is its command", NULL);
    {
        /* Service_StartWimp is claimed with the command, and Service_Reset
         * frees the workspace.  Reset first, as the desktop going does. */
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = SERVICE_RESET;
        m->service(m, &s);
        uint32_t depth = ros_call_depth;
        ros_call_depth = depth + 1;
        /* not started unless asked for (rosgd.deskmeter): a probe's icon bar stays still */
        deskmeter_autostart = 0;
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = SERVICE_STARTWIMP;
        m->service(m, &s);
        check(s.r[1] == SERVICE_STARTWIMP && ros_ld32(m->private_word) == 0,
              "Service_StartWimp passed on when not asked for", NULL);
        deskmeter_autostart = 1;
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = SERVICE_STARTWIMP;
        m->service(m, &s);
        deskmeter_autostart = -1;
        ros_call_depth = depth;
        check(s.r[1] == 0 && s.r[0] != 0 && strcmp(ros_ptr(s.r[0]), "DeskMeter") == 0,
              "asked for, Service_StartWimp is claimed with *DeskMeter", NULL);
        ros_cpu_enter(&s);
        s.r[1] = SERVICE_RESET;
        m->service(m, &s);
        check(ros_ld32(m->private_word) == 0, "Service_Reset frees the workspace", NULL);
    }

    ros_rma_free(block);
}
