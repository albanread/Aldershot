/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_drag.c: DragASprite and DragAnObject, compiled from ObjAsm into
 * the ROM (Desktop/DragASprit, Desktop/DragAnObj): what holds without a Wimp
 * task.  Starting a drag needs one, because Wimp_DragBox refuses a caller
 * that is not a task.  So tests/desktop/drag holds the rest, against
 * RISC OS 5.30.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "rom_dragasprite.h"
#include "selftest.h"

#define check ros_check

#define XDRAGASPRITE_STOP  0x62401u
#define XDRAGANOBJECT_STOP 0x69C41u

static struct ros_module *named(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (m->title && strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

/* The probes' checksum (tests/desktop/drag/modules.bas): each word, the
 * total rotated left one bit first */
static uint32_t checksum(uint32_t base, uint32_t size)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 4 <= size; i += 4)
        sum = (sum << 1 | sum >> 31) ^ ros_ld32(base + i);
    return sum;
}

/* A SWI with R0-R9 set to 1-10: whether they came back so, V clear */
static int preserves(uint32_t swi)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    for (int i = 0; i < 10; i++)
        s.r[i] = (uint32_t)i + 1;
    ros_swi(&s, swi);
    int ok = !s.v;
    for (int i = 0; i < 10; i++)
        ok &= s.r[i] == (uint32_t)i + 1;
    return ok;
}

static const char *refused(uint32_t swi, uint32_t *errnum)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, ROS_X_BIT | swi);
    if (!s.v)
        return "";
    const os_error *e = ros_ptr(s.r[0]);
    *errnum = e->errnum;
    return e->errmess;
}

void ros_selftest_drag(void)
{
    struct ros_module *das = named("DragASprite"), *dao = named("DragAnObject");
    int started = das && dao && das->base == 0xFCA00000u && dao->base == 0xFCA80000u &&
                  ros_module_version(das) == 0x2100 && ros_module_version(dao) == 0x1000 &&
                  das->private_word && ros_ld32(das->private_word) &&
                  das->swi_chunk == 0x42400 && dao->swi_chunk == 0x49C40;
    check(started, "DragASprite 0.21 and DragAnObject 0.10 start from the ROM; DragASprite "
          "claims its workspace", NULL);
    if (!started)
        return;

    /* The images as assembled are RISC OS 5.30's own, byte for byte.  These
     * are the farm's checksums of the modules in its ROM. */
    uint32_t a = checksum(das->base, 6072), b = checksum(dao->base, 1320);
    check(a == 0x2717B07Bu && b == 0x1D53422Cu, "DragASprite, DragAnObject -- the images "
          "RISC OS 5.30's ROM holds", "&%08X &%08X", a, b);

    /* While a drag runs the Wimp calls these, by the addresses in the drag
     * block: each must be compiled code the dispatcher knows */
    check(ros_code_lookup(GETS_Plot) && ros_code_lookup(GETS_Move) &&
          ros_code_lookup(GETS_UnPlot), "DragASprite's Plot, Move and UnPlot are entries",
          NULL);

    int stop = preserves(XDRAGASPRITE_STOP) && preserves(XDRAGANOBJECT_STOP) &&
               ros_ld32(ros_ld32(das->private_word) + 4) == 0;
    check(stop, "DragASprite_Stop, DragAnObject_Stop with no drag -- no error, R0-R9 kept, no "
          "sprite areas", NULL);

    uint32_t n1 = 0, n2 = 0;
    const char *m1 = refused(0x42402, &n1), *m2 = refused(0x49C7F, &n2);
    check(n1 == 0x1E6 && strcmp(m1, "SWI value out of range for module DragASprite") == 0 &&
          n2 == 0x1E6 && strcmp(m2, "SWI value out of range for module DragAnObject") == 0,
          "SWIs past each chunk's end -- &1E6, the module named", "&%X %s; &%X %s", n1, m1,
          n2, m2);
}
