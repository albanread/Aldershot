/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_display.c: the Display Manager and the mode list beneath
 * it: OS_ScreenMode 2's enumeration of the display's modes, the mode
 * strings that reasons 13 and 14 make and read, ScreenModes' ReadInfo, and
 * the module's services and command.  All of this runs before there is a
 * desktop to start the task.  The deskprobes (tests/deskprobe) check what
 * the task itself does on the desktop, against RISC OS 5.30.
 */
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "resourcefs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "scrmodes.h"
#include "selftest.h"

#define check ros_check

#define SERVICE_STARTWIMP 0x49u
#define SERVICE_RESET     0x27u

static uint32_t line;                       /* arena: strings in, results out */

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

static struct ros_module *module(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (m->title && strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

void ros_selftest_display(void)
{
    line = ros_addr(ros_rma_alloc(512));

    /* ---- OS_ScreenMode 2: counting ---------------------------------- */
    uint32_t r[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };
    check(!swi(XOS_ScreenMode, r), "OS_ScreenMode 2 counts", NULL);
    uint32_t count = (uint32_t)-(int32_t)r[2];
    uint32_t size = -r[7];
    check(count > 0 && size > 0, "the display's modes are enumerated",
          "%u modes, %u bytes", count, size);

    /* the block filled, and what it holds: each descriptor valid, a size
     * that lands on the next, a name "W x H", and the display's sizes */
    uint32_t table = ros_addr(ros_rma_alloc(size + 4));
    r[0] = 2, r[2] = 0, r[6] = table, r[7] = size;
    check(!swi(XOS_ScreenMode, r) && r[1] != 0, "the block is filled", NULL);
    uint32_t at = table, n = 0, classes = 0;
    uint32_t lastx = 0, lasty = 0;
    int mono = 0, colour256 = 0, c16m = 0, c64k = 0, format1 = 0;
    while (n < count) {
        uint32_t blocksize = ros_ld32(at);
        uint32_t flags = ros_ld32(at + 4);
        uint32_t xres = ros_ld32(at + 8), yres = ros_ld32(at + 12);
        check((flags & 0xFFu) == 1 || (flags & 0xFFu) == 3, "descriptor is valid", "%u", n);
        check(blocksize >= 24 && at + blocksize <= table + size, "descriptor has a size", "%u", n);
        const char *name = (const char *)ros_ptr(at + ((flags & 2) ? 32 : 24));
        char want[24];
        snprintf(want, sizeof want, "%u x %u", xres, yres);
        (void)want;
        check(strcmp(name, want) == 0, "descriptor is named as its size", "%u \"%s\"", n, want);
        uint32_t depth = (flags & 2) ? ros_ld32(at + 24) : ros_ld32(at + 16);
        uint32_t ncolour = (flags & 2) ? ros_ld32(at + 16) :
                           depth >= 5 ? 0xFFFFFFFFu : (1u << (1u << depth)) - 1;
        uint32_t modeflags = (flags & 2) ? ros_ld32(at + 20) : 0;
        if (depth == 0 && ncolour == 1)
            mono = 1;
        if (depth == 3 && ncolour == 255)
            colour256 = 1;
        if (depth == 5)
            c16m = 1;
        if (depth == 4 && ncolour == 65535 && (modeflags & 0x80u)) {
            c64k = 1;
            format1 = 1;                     /* the old six have no 64K */
            check((flags & 2) != 0, "64K comes as a pixel format", NULL);
        }
        if (xres != lastx || yres != lasty)
            classes++;
        lastx = xres, lasty = yres;
        at += blocksize;
        n++;
    }
    check(mono && colour256 && c16m, "the depths run from mono to 16 million", NULL);
    check(c64k, "the 64K format is offered", NULL);
    check(format1, "a format beyond the old six is format 1", NULL);
    check(r[6] == at, "the block ends at the last descriptor", NULL);

    /* a short block ends the enumeration, with R1 0 */
    r[0] = 2, r[2] = 0, r[6] = table, r[7] = 8;
    check(!swi(XOS_ScreenMode, r) && r[1] == 0, "a short block stops the enumeration", NULL);

    /* the modes are the display's: one class a size */
    check(classes >= 1, "the resolutions are the display's", "%u classes", classes);

    /* ---- OS_ScreenMode 14 and 13: the mode strings ------------------ */
    uint32_t sel = ros_addr(ros_rma_alloc(64));
    uint32_t out = ros_addr(ros_rma_alloc(64));
    /* C16M */
    ros_st32(sel, 1), ros_st32(sel + 4, 1024), ros_st32(sel + 8, 768);
    ros_st32(sel + 12, 5), ros_st32(sel + 16, 60);
    ros_st32(sel + 20, 3), ros_st32(sel + 24, 0xFFFFFFFFu);    /* NColour */
    ros_st32(sel + 28, 0), ros_st32(sel + 32, 0);              /* ModeFlags */
    ros_st32(sel + 36, 0xFFFFFFFFu);
    r[0] = 14, r[1] = sel, r[2] = out, r[3] = 64;
    check(!swi(XOS_ScreenMode, r) && r[3] == 0, "a specifier becomes a string", NULL);
    check(strcmp(ros_ptr(out), "X1024 Y768 C16M F60") == 0,
          "the string is the kernel's", "\"%s\"", (const char *)ros_ptr(out));
    /* 64K: NColour and the 64k flag pick C64K */
    ros_st32(sel + 12, 4);
    ros_st32(sel + 24, 65535);
    ros_st32(sel + 32, 0x80u);                             /* 64k */
    r[0] = 14, r[1] = sel, r[2] = out, r[3] = 64;
    check(!swi(XOS_ScreenMode, r), "64K becomes a string", NULL);
    check(strcmp(ros_ptr(out), "X1024 Y768 C64K F60") == 0,
          "the 64K token is C64K", "\"%s\"", (const char *)ros_ptr(out));
    /* a short buffer answers the size needed */
    r[0] = 14, r[1] = sel, r[2] = out, r[3] = 4;
    check(!swi(XOS_ScreenMode, r) && (int32_t)r[3] < 0, "a short buffer answers its size", NULL);

    /* the string read back */
    r[0] = 13, r[1] = str(line, "X1024 Y768 C64K F60"), r[2] = sel, r[3] = 64;
    check(!swi(XOS_ScreenMode, r), "a string becomes a specifier", NULL);
    check(ros_ld32(sel + 4) == 1024 && ros_ld32(sel + 8) == 768 &&
          ros_ld32(sel + 12) == 4 && ros_ld32(sel + 16) == 60,
          "the specifier is the string's", NULL);
    int has64k = 0;
    for (uint32_t p = sel + 20; ros_ld32(p) != 0xFFFFFFFFu; p += 8)
        if (ros_ld32(p) == 0 && ros_ld32(p + 4) == 0x80u)
            has64k = 1;
    check(has64k, "the 64k flag is a mode variable", NULL);
    /* the round trip */
    r[0] = 14, r[1] = sel, r[2] = out, r[3] = 64;
    check(!swi(XOS_ScreenMode, r) && strcmp(ros_ptr(out), "X1024 Y768 C64K F60") == 0,
          "the string round trips", NULL);
    /* a mode number */
    r[0] = 13, r[1] = str(line, "28"), r[2] = sel, r[3] = 64;
    check(!swi(XOS_ScreenMode, r) && ros_ld32(sel + 4) == 640 && ros_ld32(sel + 8) == 480,
          "a mode number becomes its specifier", NULL);
    /* the refusals */
    r[0] = 13, r[1] = str(line, "X1024"), r[2] = sel, r[3] = 64;
    check(swi(XOS_ScreenMode, r), "a string without Y and C is refused", NULL);
    r[0] = 13, r[1] = str(line, "X1024 Y768 Q17"), r[2] = sel, r[3] = 64;
    check(swi(XOS_ScreenMode, r), "an unknown token is refused", NULL);
    r[0] = 13, r[1] = str(line, "X1024 X768 C16M"), r[2] = sel, r[3] = 64;
    check(swi(XOS_ScreenMode, r), "a repeated token is refused", NULL);
    r[0] = 14, r[1] = 28, r[2] = out, r[3] = 64;
    check(swi(XOS_ScreenMode, r), "a mode number is not a specifier", NULL);
    /* too small a buffer: Buffer overflow, and nothing written, not even
     * the flags word, so R2 = 0 with R3 = 0 asks only the size (#58) */
    ros_st32(sel, 0x12345678u);
    r[0] = 13, r[1] = str(line, "X640 Y480 C256"), r[2] = sel, r[3] = 8;
    int o1 = swi(XOS_ScreenMode, r) && ros_ld32(r[0]) == 0x1E4 && ros_ld32(sel) == 0x12345678u;
    r[0] = 13, r[1] = str(line, "X640 Y480 C256"), r[2] = 0, r[3] = 0;
    int o2 = swi(XOS_ScreenMode, r) && ros_ld32(r[0]) == 0x1E4;
    check(o1 && o2, "a short buffer is Buffer overflow before anything is written (#58)",
          "%d %d &%08X", o1, o2, ros_ld32(sel));

    /* a mode number's selector keeps only the pixel format's ModeFlags, as
     * the kernel's ScreenMode_ModeStringToSpecifier: the farm (RISC OS 5.30)
     * gives &80 for mode 7 and 0 for mode 14 (#73) */
    {
        static const uint32_t want[][2] = { { 7, 0x80 }, { 14, 0 } };
        for (unsigned i = 0; i < 2; i++) {
            char num[4];
            snprintf(num, sizeof num, "%u", want[i][0]);
            r[0] = 13, r[1] = str(line, num), r[2] = sel, r[3] = 64;
            uint32_t flags = 0xFFFFFFFFu;
            if (!swi(XOS_ScreenMode, r))
                for (uint32_t at = sel + 20; ros_ld32(at) != 0xFFFFFFFFu && at < sel + 64; at += 8)
                    if (ros_ld32(at) == 0)          /* ModeFlags is variable 0 */
                        flags = ros_ld32(at + 4);
            check(flags == want[i][1], "a mode number's selector keeps ModeFlags' pixel-format bits",
                  "mode %u: &%X, not &%X", want[i][0], flags, want[i][1]);
        }
    }

    /* a parsed selector selects a mode, and the current mode is it */
    uint32_t was = ros_addr(ros_rma_alloc(64));
    r[0] = 1;
    check(!swi(XOS_ScreenMode, r), "the current mode reads", NULL);
    memcpy(ros_ptr(was), ros_ptr(r[1]), 64);
    r[0] = 13, r[1] = str(line, "X640 Y480 C256"), r[2] = sel, r[3] = 64;
    check(!swi(XOS_ScreenMode, r), "X640 Y480 C256 parses", NULL);
    uint32_t s2[8] = { 0, sel };
    check(!swi(XOS_ScreenMode, s2), "the mode is selected", NULL);
    r[0] = 1;
    check(!swi(XOS_ScreenMode, r) && ros_ld32(r[1] + 4) == 640 && ros_ld32(r[1] + 8) == 480,
          "OS_ScreenMode 1 gives the new mode's selector", NULL);
    s2[0] = 0, s2[1] = was;
    check(!swi(XOS_ScreenMode, s2), "the mode returns", NULL);

    /* ---- ScreenModes ------------------------------------------------- */
    struct ros_module *sm = module("ScreenModes");
    check(sm != NULL, "ScreenModes is in the chain", NULL);
    r[0] = 0;
    check(!swi(0x487C0u | 0x20000u, r), "ScreenModes_ReadInfo 0 answers", NULL);
    check(strlen(ros_ptr(r[0])) > 0, "the monitor is named", "\"%s\"", (const char *)ros_ptr(r[0]));
    r[0] = 99;
    check(swi(0x487C0u | 0x20000u, r), "an unknown ReadInfo is refused", NULL);

    /* ---- the DisplayManager module ----------------------------------- */
    struct ros_module *m = module("DisplayManager");
    check(m != NULL, "DisplayManager is in the chain", NULL);
    check(m->commands && strcmp(m->commands[0].name, "Desktop_DisplayManager") == 0,
          "*Desktop_DisplayManager is its command", NULL);

    /* DisplayManager$Path: Mod_Init sets its default if the variable is
     * not there.  The suites before this one rewrite the variable store.
     * So this tests the mechanism: set the variable, as Mod_Init does, and
     * read it back. */
    strcpy(ros_ptr(line + 64), "Resources:$.Resources.Display.");
    r[0] = str(line, "DisplayManager$Path"), r[1] = line + 64, r[2] = 31, r[3] = 0,
    r[4] = 0;                                   /* VarType_String: 0 */
    check(!swi(XOS_SetVarVal, r), "DisplayManager$Path is set", NULL);
    r[0] = str(line, "DisplayManager$Path"), r[1] = line + 128, r[2] = 256, r[3] = 0,
    r[4] = 0;
    {
        int ok = !swi(XOS_ReadVarVal, r) && r[2] == 30;
        ros_st8(line + 128 + r[2], 0);         /* the terminator the kernel writes */
        check(ok && strcmp(ros_ptr(line + 128), "Resources:$.Resources.Display.") == 0,
              "DisplayManager$Path reads its value back", NULL);
    }

    /* its resources in ResourceFS */
    check(ros_resourcefs_find("Resources:$.Resources.Display.Templates") != 0,
          "the templates are in ResourceFS", NULL);
    check(ros_resourcefs_find("Resources:$.Resources.Display.Messages") != 0,
          "the messages are in ResourceFS", NULL);

    /* The task's menus.  Each resolution item must map to its own class and
     * back.  (The menu list points into the class list, and a choice once
     * read past the end of it and the box powered off.)  Each choice at 16
     * million and 256 colours must give a mode string for *WimpMode.  (The
     * NColour for 16 million was once 0.) */
    {
        char why[80] = "";
        check(ros_display_check_menus(why, sizeof why) == 0,
              "every resolution choice is a mode *WimpMode takes", "%s", why);
    }

    /* Service_StartWimp is claimed with the command, and Service_Reset
     * frees the workspace.  The Wimp, started by an earlier suite, may have
     * claimed already, so Reset first, as the desktop going does. */
    {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = SERVICE_RESET;
        m->service(m, &s);
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = SERVICE_STARTWIMP;
        uint32_t depth = ros_call_depth;
        ros_call_depth = depth + 1;
        m->service(m, &s);
        ros_call_depth = depth;
        check(s.r[1] == 0 && s.r[0] != 0 &&
              strcmp(ros_ptr(s.r[0]), "Desktop_DisplayManager") == 0,
              "Service_StartWimp is claimed with *Desktop_DisplayManager", NULL);
        check(ros_ld32(m->private_word) != 0, "the workspace is claimed", NULL);
        /* the second claim changes nothing */
        uint32_t ws = ros_ld32(m->private_word);
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = SERVICE_STARTWIMP;
        m->service(m, &s);
        check(ros_ld32(m->private_word) == ws, "a running task is not restarted", NULL);
        ros_cpu_enter(&s);
        s.r[1] = SERVICE_RESET;
        m->service(m, &s);
        check(ros_ld32(m->private_word) == 0, "Service_Reset frees the workspace", NULL);
    }

    ros_rma_free(ros_ptr(table));
    ros_rma_free(ros_ptr(sel));
    ros_rma_free(ros_ptr(out));
    ros_rma_free(ros_ptr(was));
}
