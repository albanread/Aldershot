/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_deskclock.c: the Desk Clock module, and the sprite pipeline
 * it draws with: an area of our own, a sprite created in the current
 * mode, output switched to it through a save area, a plot, and back.
 * All of this runs before there is a desktop to start the task.  What the
 * clock looks like on the bar is the deskprobes'.
 */
#include <stdio.h>
#include <string.h>

#include "deskclock.h"
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

void ros_selftest_deskclock(void)
{
    /* ---- the module ------------------------------------------------- */
    struct ros_module *m = module("DeskClock");
    check(m != NULL, "DeskClock is in the chain", NULL);
    check(m && m->commands && strcmp(m->commands[0].name, "DeskClock") == 0,
          "*DeskClock is its command", NULL);

    /* Service_StartWimp is claimed with the command, and Service_Reset
     * frees the workspace.  The Wimp may have claimed already, so Reset
     * first, as the desktop going does. */
    {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = SERVICE_RESET;
        m->service(m, &s);
        uint32_t depth = ros_call_depth;
        ros_call_depth = depth + 1;
        /* not started unless asked for (rosgd.deskclock): a probe's icon bar stays still */
        deskclock_autostart = 0;
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = SERVICE_STARTWIMP;
        m->service(m, &s);
        check(s.r[1] == SERVICE_STARTWIMP && ros_ld32(m->private_word) == 0,
              "Service_StartWimp passed on when not asked for", NULL);
        deskclock_autostart = 1;
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = SERVICE_STARTWIMP;
        m->service(m, &s);
        deskclock_autostart = -1;
        ros_call_depth = depth;
        check(s.r[1] == 0 && s.r[0] != 0 && strcmp(ros_ptr(s.r[0]), "DeskClock") == 0,
              "asked for, Service_StartWimp is claimed with *DeskClock", NULL);
        check(ros_ld32(m->private_word) != 0, "the workspace is claimed", NULL);
        ros_cpu_enter(&s);
        s.r[1] = SERVICE_RESET;
        m->service(m, &s);
        check(ros_ld32(m->private_word) == 0, "Service_Reset frees the workspace", NULL);
    }

    /* ---- the sprite pipeline the clock draws with ------------------- */
    {
        uint32_t *area = ros_rma_alloc(1024);
        check(area != NULL, "a sprite area is claimed", NULL);
        memset(area, 0, 1024);
        /* the header words as the kernel's SpriteOp 9 writes them */
        ros_st32(ros_addr(area), 1024);
        ros_st32(ros_addr(area) + 4, 0);
        ros_st32(ros_addr(area) + 8, 16);
        ros_st32(ros_addr(area) + 12, 16);
        uint32_t r[8] = { 0x100 | 9, ros_addr(area), 1024, 1 };
        check(!swi(XOS_SpriteOp, r), "SpriteOp 9 initialises the area", NULL);

        uint32_t mode = 0, xeig = 1, yeig = 1;
        r[0] = 1;
        if (!swi(XOS_ScreenMode, r))
            mode = r[1];
        r[0] = mode, r[1] = 4;
        if (!swi(XOS_ReadModeVariable, r))
            xeig = r[2];
        r[0] = mode, r[1] = 5;
        if (!swi(XOS_ReadModeVariable, r))
            yeig = r[2];
        uint32_t sprite_mode = 15u << 27 | 6u << 20 | 1u | xeig << 4 | yeig << 6;
        strcpy(ros_ptr(ros_addr(area) + 512), "test");
        r[0] = 0x100 | 15, r[1] = ros_addr(area), r[2] = ros_addr(area) + 512,
        r[3] = 0, r[4] = 8, r[5] = 8, r[6] = sprite_mode;
        int made = !swi(XOS_SpriteOp, r);
        check(made, "SpriteOp 15 creates a 32bpp sprite at this mode's scale",
              "word &%X: %s", sprite_mode, made ? "" : ros_ptr(r[0] + 4));

        r[0] = 0x100 | 24, r[1] = ros_addr(area), r[2] = ros_addr(area) + 512;
        int chose = made && !swi(XOS_SpriteOp, r) && r[2] != ros_addr(area) + 512;
        check(chose, "SpriteOp 24 selects it, and says where it is", NULL);
        uint32_t sp = r[2];

        /* the face's way of drawing: the pixels written straight into
         * the image.  No VDU output is switched (SpriteOp 60 and 61 would
         * race the pointer's vsync).  The plotter's way is not tested here. */
        uint32_t img = sp + 32;                  /* a sprite header: image at +32 */
        uint32_t words = ros_ld32(sp + 16) + 1;  /* words a row (the header holds one less) */
        check(words == 8, "the 8x8 32bpp sprite is eight words a row", "%u", words);
        ros_st32(img + (4 * words + 4) * 4, 0x00E0F2F8u);   /* a face's ivory */
        check(ros_ld32(img + (4 * words + 4) * 4) == 0x00E0F2F8u,
              "the pixel written into the image is there", NULL);

        ros_rma_free(area);
    }

    /* ---- the clock's own sprite (#13): an old-format 4 bpp sprite with a
     * palette and a mask made by SpriteOp 29.  The mask is inside the sprite,
     * is the image's size, and has every pixel solid.  SpriteExtend plots it
     * in the sprite's own colours and leaves a masked-out pixel as it was. */
    {
        uint32_t *area = ros_rma_alloc(4096);
        check(area != NULL, "a sprite area is claimed", NULL);
        memset(area, 0, 4096);
        uint32_t a = ros_addr(area);
        ros_st32(a, 3072), ros_st32(a + 8, 16), ros_st32(a + 12, 16);
        uint32_t r[8] = { 0x100 | 9, a };
        swi(XOS_SpriteOp, r);
        strcpy(ros_ptr(a + 3584), "face");
        r[0] = 0x100 | 15, r[1] = a, r[2] = a + 3584, r[3] = 1, r[4] = 34, r[5] = 34, r[6] = 27;
        int made = !swi(XOS_SpriteOp, r);
        r[0] = 0x100 | 24, r[1] = a, r[2] = a + 3584;
        made = made && !swi(XOS_SpriteOp, r);
        uint32_t sp = r[2];
        check(made && ros_ld32(sp + 32) == 44 + 16 * 8,
              "a 4 bpp mode-27 sprite with a palette: its image after sixteen entries", NULL);
        for (int i = 0; i < 16; i++)                    /* entry 0 &563412, the rest &FFFFFF */
            ros_st32(sp + 44 + 8 * i, i ? 0xFFFFFF00u : 0x56341200u),
            ros_st32(sp + 48 + 8 * i, i ? 0xFFFFFF00u : 0x56341200u);
        uint32_t image = ros_ld32(sp) - ros_ld32(sp + 32);         /* no mask yet */
        r[0] = 0x100 | 29, r[1] = a, r[2] = a + 3584;
        int masked = !swi(XOS_SpriteOp, r);
        r[0] = 0x100 | 24, r[1] = a, r[2] = a + 3584;
        swi(XOS_SpriteOp, r);
        sp = r[2];
        uint32_t trans = ros_ld32(sp + 36), next = ros_ld32(sp);
        int solid = 1;
        for (uint32_t i = 0; i < image && masked; i++)
            solid &= ros_ld8(sp + trans + i) == 0xFF;
        check(masked && trans == ros_ld32(sp + 32) + image && next == trans + image && solid,
              "SpriteOp 29: the mask inside the sprite, the image's size, all solid",
              "image %u trans %u next %u", image, trans, next);
        ros_st8(sp + trans, 0xF0);                      /* pixel 0 of the top row: clear */

        uint32_t v[8] = { 0 };
        uint32_t corner_before = 0;
        v[0] = 0, v[1] = 34 * 2 - 2;                    /* the top left pixel, 2 OS units a pixel */
        if (!swi(XOS_ReadPoint, v))
            corner_before = v[2];
        r[0] = 0x200 | 52, r[1] = a, r[2] = sp, r[3] = 0, r[4] = 0, r[5] = 8 | 16, r[6] = 0, r[7] = 0;
        int plotted = !swi(XOS_SpriteOp, r);
        v[0] = 0, v[1] = 34 * 2 - 2;
        uint32_t corner = swi(XOS_ReadPoint, v) ? 0xDEAD : v[2];
        v[0] = 34, v[1] = 34;
        uint32_t centre = swi(XOS_ReadPoint, v) ? 0xDEAD : v[2];
        check(plotted && centre == 0x563412u && corner == corner_before,
              "SpriteExtend plots it: the palette's colour, the masked pixel untouched",
              "%s centre &%X corner &%X (was &%X)", plotted ? "" : (char *)ros_ptr(r[0] + 4),
              centre, corner, corner_before);
        ros_rma_free(area);
    }
}
