/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_desktop.c: the Desktop and WindowUtils, compiled from ObjAsm
 * into the ROM: what must hold before the Wimp runs tasks.  The modules
 * are there, as RISC OS 5.30's ROM has them.  *Help reads the Desktop's
 * help from its Messages file.  Its resources are in ResourceFS.
 * tests/desktop/desktop checks what the Desktop does as a Wimp task,
 * against the farm. */
#include <string.h>

#include "resourcefs.h"
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

static uint32_t scratch;            /* arena: strings in, results out */
static char out[1024];
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

/* OS_CLI, its output caught: 0 or the error number */
static uint32_t cli(const char *cmd)
{
    outn = 0, out[0] = 0;
    uint32_t r[8] = { str(scratch, cmd) };
    ros_vector_claim_native(WRCHV, wrch, 0);
    int v = swi(XOS_CLI, r);
    ros_vector_release_native(WRCHV, wrch, 0);
    return v ? ((os_error *)ros_ptr(r[0]))->errnum : 0;
}

/* A module by name (OS_Module 18): its base, private word and version */
static int module(const char *name, uint32_t *base, uint32_t *word, uint32_t *version)
{
    uint32_t r[8] = { 18, str(scratch, name) };
    if (swi(XOS_Module, r))
        return 0;
    *base = r[3], *word = r[4], *version = 0;
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (m->base == r[3])
            *version = ros_module_version(m);
    return 1;
}

/* A text window 80 columns wide (VDU 28), or the one it replaced back:
 * OS_PrettyPrint wraps at the window's width. */
static void window80(uint32_t was[4], int on)
{
    uint8_t v[5] = { 28, 0, 24, 79, 0 };
    if (on) {
        uint32_t in = scratch + 400, res = in + 20;
        ros_st32(in, 128), ros_st32(in + 4, 129), ros_st32(in + 8, 130), ros_st32(in + 12, 131);
        ros_st32(in + 16, (uint32_t)-1);
        uint32_t r[8] = { in, res };        /* ScrLCol, ScrBRow, ScrRCol, ScrTRow */
        swi(XOS_ReadVduVariables, r);
        for (int i = 0; i < 4; i++)
            was[i] = ros_ld32(res + 4 * (uint32_t)i);
    } else {
        for (int i = 0; i < 4; i++)
            v[i + 1] = (uint8_t)was[i];
    }
    for (int i = 0; i < 5; i++) {
        uint32_t r[8] = { v[i] };
        swi(XOS_WriteC, r);
    }
}

/* What RISC OS 5.30 prints for *Help Desktop (tests/desktop/desktop/expected/help.txt):
 * paged mode on and off around it, the help wrapped at 80 columns. */
static const char help_desktop[] =
    "\x0e\r==> Help on keyword Desktop\n\r"
    "*Desktop starts up any dormant Wimp modules, and also passes an optional\n\r"
    "*command or file of *commands to Wimp_StartTask.\n\r"
    "Syntax: *Desktop [<*command> | -File <filename>]\n\r"
    "\r==> Help on keyword Desktop\n\r"
    "Module is: Desktop         2.77 (26 Oct 2018)\n\r"
    "\n\rCommands provided:\n\rDesktop\n\r\x0f";

void ros_selftest_desktop(void)
{
    scratch = ros_addr(ros_rma_alloc(512));

    uint32_t base, word, version, ubase, uword, uversion;
    int m1 = module("Desktop", &base, &word, &version) && base == 0xFC900000u && word == 0 &&
             version == 0x27700;
    int m2 = module("WindowUtils", &ubase, &uword, &uversion) && ubase == 0xFC980000u &&
             uword == 0 && uversion == 0x25300;
    int m3 = m1 && ros_ld32(base) == 0x154 && ros_ld32(base + 0x18) == 0x5C &&
             strcmp(ros_ptr(base + ros_ld32(base + 0x2C)),
                    "Resources:$.Resources.Desktop.Messages") == 0;
    check(m1 && m2 && m3,
          "Desktop 2.77 and WindowUtils 2.53 in the ROM, at &FC900000 and &FC980000 -- no "
          "workspace; the Desktop's start entry, *Desktop, its Messages file",
          "&%X &%X &%X, &%X &%X &%X", base, word, version, ubase, uword, uversion);

    uint32_t was[4];
    window80(was, 1);
    int h1 = cli("Help Desktop") == 0 && strcmp(out, help_desktop) == 0;
    window80(was, 0);
    check(h1, "*Help Desktop -- help and syntax from the Desktop's Messages, as RISC OS 5.30 "
              "prints them; then the module's summary", "\"%s\"", out);

    int r1 = cli("RMEnsure WindowUtils 2.53 Echo old") == 0 && !out[0] &&
             cli("RMEnsure WindowUtils 2.54 Echo old") == 0 && strcmp(out, "old\n\r") == 0 &&
             cli("RMEnsure Desktop 2.78") == 0x10F;
    check(r1, "*RMEnsure -- WindowUtils 2.53 is new enough, 2.54 is not; Desktop 2.78 is too old",
          "\"%s\"", out);

    /* its Messages, as the Desktop looks its title up */
    uint32_t block = scratch + 256;
    uint32_t o[8] = { block, str(scratch + 300, "Resources:$.Resources.Desktop.Messages"), 0 };
    int s1 = !swi(XMessageTrans_OpenFile, o);
    uint32_t l[8] = { block, str(scratch + 300, "Desktop"), scratch + 400, 64 };
    s1 = s1 && !swi(XMessageTrans_Lookup, l) && l[3] == 7 &&
         memcmp(ros_ptr(scratch + 400), "Desktop", 8) == 0;
    uint32_t c[8] = { block };
    swi(XMessageTrans_CloseFile, c);
    uint32_t sprites = ros_resourcefs_find("Resources:$.Resources.Desktop.Sprites");
    uint32_t templates = ros_resourcefs_find("Resources:$.Resources.Desktop.Templates");
    int s2 = sprites && memcmp(ros_ptr(sprites + 4), "SQSH", 4) == 0 &&
             ros_ld32(sprites + 8) == 32804 && ros_ld8(sprites + 24) == 0x1F &&
             ros_ld8(sprites + 25) == 0x9D && ros_ld8(sprites + 26) == 0x8C;
    int s3 = templates && ros_ld32(templates) == 626 + 4;
    check(s1 && s2 && s3,
          "The Desktop's resources -- its Messages through MessageTrans; the banner's sprites "
          "squashed as RISC OS's build squashes them, 32804 bytes in 12-bit LZW; its Templates",
          "%d %d %d", s1, s2, s3);

    ros_rma_free(ros_ptr(scratch));
}
