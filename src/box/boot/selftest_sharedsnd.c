/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_sharedsnd.c: SharedSound: the handler table, the SWIs, and
 * the fraction arithmetic. The handlers are arena addresses (translated
 * code), and a C function in /init is a host pointer and cannot be one. So
 * the table is tested with a word in the RMA standing in for a handler.
 * The fill calling real translated code is the desktop probe's. */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"
#include "sharedsnd.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

void ros_selftest_sharedsnd(void)
{
    struct ros_module *m = &sharedsnd_module;

    /* ---- the module is in the chain, walkable, with its chunk ---------- */
    {
        int found = 0;
        for (struct ros_module *c = ros_module_first(); c; c = c->next)
            if (c == m)
                { found = 1; break; }
        check(found, "SharedSound is in the module chain (walked)", NULL);
        check(m->swi_chunk == 0x4B440, "its SWI chunk is &4B440", "%X", m->swi_chunk);
        check(m->swi_count > 0, "its SWI count is set", "%u", m->swi_count);
    }
    check(m->private_word != 0 && ros_ld32(m->private_word) != 0,
          "the workspace is claimed at init", "pw=%08X val=%08X",
          m->private_word, m->private_word ? ros_ld32(m->private_word) : 0);

    uint32_t *rma = ros_rma_alloc(256);
    check(rma != NULL, "a test block is claimed", NULL);
    uint32_t fake = ros_addr(rma) + 64;         /* an arena address */

    /* ---- Info first: the simplest SWI, no arguments ------------------- */
    uint32_t r[8];
    {
        r[0] = 0;
        int e = swi(XSharedSound_Info, r);
        check(!e, "Info answers", "e=%d err=%u msg=%s",
              e, e && r[0] >= 0x8000u ? ros_ld32(r[0]) : 0,
              e && r[0] >= 0x8000u ? ros_ptr(r[0] + 4) : "");
        check(!e && r[0] == 44100, "Info: the system rate", "%u", r[0]);
    }

    /* ---- a handler installed, found, removed -------------------------- */
    {
        /* a dummy in slot 0: 0 means "the system" in the per-handler SWIs */
        r[0] = fake;
        r[1] = 0xDEAD;
        r[2] = 0, r[3] = 0, r[4] = 0;
        check(!swi(XSharedSound_InstallHandler, r), "a dummy installs first", NULL);

        r[0] = fake;
        r[1] = 0x12345678;
        r[2] = 0;
        r[3] = ros_addr(rma);
        r[4] = 0;
        int e = swi(XSharedSound_InstallHandler, r);
        check(!e, "a handler installs", "e=%d r0=%X", e, r[0]);
        if (e) {
            ros_rma_free(rma);
            return;
        }
        uint32_t num = r[0];
        check(num < 10, "it gets a number under 10", "%u", num);

        r[0] = num;
        r[4] = 0;
        check(!swi(XSharedSound_HandlerInfo, r), "its info reads", NULL);
        check(r[1] == fake && r[2] == 0x12345678,
              "the address and parameter come back", NULL);

        r[0] = num;
        check(!swi(XSharedSound_HandlerType, r) && r[0] == 0,
              "its type is immediate (0)", "%u", r[0]);

        if (num) {
            r[0] = num, r[1] = 0x8000;
            check(!swi(XSharedSound_HandlerVolume, r) && r[0] == 0x8000,
                  "its volume reads back after a set", "%X", r[0]);
        }

        r[0] = 22050, r[1] = num;
        check(!swi(XSharedSound_SampleRate, r), "its rate sets", NULL);
        r[0] = 0;
        check(!swi(XSharedSound_SampleRate, r), "the system rate reads", NULL);
        check(r[1] == 44100, "the system rate is 44100", "%u", r[1]);
        /* R2 is the SYSTEM fraction (1:1); the handler's own fraction is
         * its handler table entry, not what this SWI returns */
        check(r[2] == 0x10000, "the system fraction is 1:1", "%X", r[2]);

        r[0] = num;
        check(!swi(XSharedSound_RemoveHandler, r), "it removes", NULL);
        r[0] = fake;
        r[1] = 0xDEAD;
        swi(XSharedSound_RemoveHandler, r);
        r[0] = num;
        check(swi(XSharedSound_HandlerType, r), "its type now errors", NULL);
    }

    /* ---- the errors ------------------------------------------------------ */
    {
        r[0] = 0;                                 /* no address */
        check(swi(XSharedSound_InstallHandler, r), "a zero address is refused", NULL);

        r[0] = 99;                                /* no such handler */
        check(swi(XSharedSound_HandlerType, r), "a bad number is refused", NULL);

        r[0] = 1;                                 /* the only driver */
        check(swi(XSharedSound_RemoveDriver, r), "the driver cannot be removed", NULL);

        /* a handler number with bit 31 set is a number out of range, not a
         * negative index into the workspace before the table */
        r[0] = 0xFFFFFFFFu, r[4] = 0;
        check(swi(XSharedSound_HandlerInfo, r), "HandlerInfo refuses a negative number", NULL);
        r[0] = 0x80000000u, r[1] = 0x1234;
        check(swi(XSharedSound_HandlerVolume, r), "HandlerVolume refuses a negative number", NULL);
        r[0] = 22050, r[1] = 0xFFFFFFFFu;
        check(swi(XSharedSound_SampleRate, r), "SampleRate refuses a negative number", NULL);
    }

    /* ---- ten handlers, then full ---------------------------------------- */
    {
        uint32_t nums[10];
        int all = 1;
        for (int i = 0; i < 10; i++) {
            r[0] = fake;
            r[1] = (uint32_t)(0x100 + i);        /* distinct parameters */
            r[2] = 0, r[3] = 0, r[4] = 0;
            if (swi(XSharedSound_InstallHandler, r))
                { all = 0; break; }
            nums[i] = r[0];
        }
        check(all, "ten handlers install", NULL);

        r[0] = fake;
        r[1] = 0x9999;                            /* an eleventh pair */
        r[2] = 0, r[3] = 0, r[4] = 0;
        check(swi(XSharedSound_InstallHandler, r),
              "the eleventh is refused", NULL);

        for (int i = 0; i < 10; i++) {
            r[0] = nums[i];
            swi(XSharedSound_RemoveHandler, r);
        }
    }

    /* ---- Info and ControlWord -------------------------------------------- */
    {
        r[0] = 0;
        check(!swi(XSharedSound_Info, r) && r[0] == 44100,
              "Info: the system rate", "%u", r[0]);
        check(!swi(XSharedSound_ControlWord, r) && r[0] >= 0x8000u,
              "ControlWord: a workspace address", "%X", r[0]);
    }

    ros_rma_free(rma);
}
