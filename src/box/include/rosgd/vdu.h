/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* vdu.h -- the VDU drivers (runtime/vdu), reimplemented.
 *
 * The kernel's VDU drivers own what is on the screen: the VDU stream that
 * OS_WriteC feeds, text and its cursor, colours and the palette, modes.
 * Under ROSGD they are native C written from the kernel's (Kernel/s/vdu),
 * pixel for pixel; the hardware beneath is GraphicsV's, DRMVideo's
 * (modules/drmvideo), which the VDU drivers call as the kernel's do.
 *
 * There is one screen, so one VDU. But a task can have a virtual display
 * of its own (below), whose VDU context the baton swaps in for
 * the task's own code.
 */
#ifndef ROSGD_VDU_H
#define ROSGD_VDU_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

/* Start the VDU drivers in the mode GraphicsV's driver starts in
 * (GraphicsV_StartupMode): after the ROM's modules, DRMVideo among them.
 * With no driver, the VDU keeps its state and draws nothing. */
void ros_vdu_init(void);

/* A character from WrchV's default owner.  *plain says whether it is one a
 * serial console should show too: printable, or BEL, BS, LF, CR. It is not
 * a VDU sequence's parameter, nor a code that only means something on the
 * screen.  An error is a mode change's, as VDU 22 reports one. */
os_error *ros_vdu_write(uint8_t ch, int *plain);

/* The 50 Hz VSync: the cursor's and the palette's flashing
 * (runtime/osbyte.c, with the lock held). */
void ros_vdu_vsync(void);

/* Cursor editing, which the keyboard hands the cursor keys and COPY when
 * OS_Byte 4's state is 0 (DoCursorEdit; runtime/keyboard.c).  The codes
 * are &87 COPY, &88 left, &89 right, &8A down, &8B up.  A cursor key
 * splits the cursors if they are not split already and moves the second
 * one; COPY reads the character under it, moves it on, and is the only
 * one that gives the caller a character: 1 and *out set, where the others
 * answer 0. The key is then used up and the read goes back to waiting. */
int ros_vdu_cursor_edit(uint32_t code, uint8_t *out);

/* For OS_ReadSysInfo 0 and 1: the framebuffer's size, the mode the
 * display started in */
uint32_t ros_vdu_screen_size(void);
uint32_t ros_vdu_start_mode(void);

/* The system sprite area moved or changed size: its address, or 0
 * (runtime/dynarea.c, which owns dynamic area 3). */
void ros_vdu_sprite_area(uint32_t area);
void ros_dynarea_init(void);
/* Whether [start, end) lies wholly in the part of one dynamic area in use */
int ros_dynarea_contains(uint32_t start, uint32_t end);

/* For SpriteExtend: the ECF origin as the kernel keeps it (ECFShift,
 * ECFYOffset, which its OS_ReadSysInfo 6 gives), and 256 bytes of RMA
 * scratch for OS_ReadVduVariables blocks. */
void ros_vdu_ecf_offsets(uint32_t *shift, uint32_t *yoffset);
uint32_t ros_vdu_scratch(void);

/* ExportedHLine (VDU variable HLineAddr), for Draw: a span in the
 * current graphics window, x0 and x1 inclusive in either order; how = 1
 * the foreground colour and action, 3 the background, 2 invert, 0 no
 * effect, else the address of eight {ora, eor} word pairs */
void ros_vdu_hline(int32_t x0, int32_t y, int32_t x1, uint32_t how);

/* The VDU's OS_Bytes and OS_Words: 1 if it was one of them. */
int ros_vdu_byte(struct ros_cpu *s);
int ros_vdu_word(struct ros_cpu *s);

/* ---- virtual displays (runtime/vdu/vdisplay.c) --------------------------
 *
 * A VDU context is the VDU's whole state; the real display has one, and so
 * does each virtual display.  One is live, in the drivers' workspace.  The
 * live one is the current task's virtual display while its own code runs,
 * and the real display's for background work, callbacks and the Wimp's
 * SWIs (real sections). */
struct ros_module;
extern struct ros_module vdisplay_module;

/* How many virtual displays there are: 0, and nothing here does anything */
extern unsigned ros_vdu_displays;

/* task.c's install(): the new current task's context made live */
void ros_vdu_task_installed(void);

/* swi.c, at each SWI's entry: user is 1 for the task's own SWI (depth 0, not
 * background).  1 if the SWI is a real section (a Wimp SWI), to be left
 * with ros_vdu_real_leave(1) as it returns. */
int ros_vdu_swi_enter(uint32_t n, int user);
/* A real section entered (1) or not (0): the way out of the outermost
 * SWI, its background work and callbacks */
int ros_vdu_real_enter(void);
void ros_vdu_real_leave(int entered);

/* background.c: the real context for background work, and back */
void *ros_vdu_background_enter(void);
void ros_vdu_background_leave(void *was);

/* Which context is live, and its mode: it changes when either does, for
 * modules that cache the screen (ColourTrans, the Font Manager) */
uint32_t ros_vdu_context_id(void);
/* The live context's output: OS_SpriteOp 60's R0-R2 as they stand */
void ros_vdu_output(uint32_t *select, uint32_t *area, uint32_t *sprite);
/* Key scanning (negative INKEY) refused: a virtual display whose window
 * has not got the input focus */
int ros_vdu_keys_blocked(void);

/* For the native Wimp's surface windows. A display as it
 * is shown: the shown bank's sprite area and sprite (its palette is brought
 * up to date when pal_gen or the bank has moved, so pass the last view
 * back in), its size in pixels, eigen factors, depth, and the generations
 * of its mode and palette. The result is 0, or -1 for no such display. */
struct ros_vdisplay_view {
    uint32_t area, sprite;
    uint32_t width, height, xeig, yeig, log2bpp;
    uint32_t mode_gen, pal_gen, pal_entries;
};
int ros_vdisplay_view(uint32_t handle, struct ros_vdisplay_view *v);
/* What changed since the last take, cleared: 0 nothing, else bit 0 and
 * (bit 1) all of it, box[] the display's pixels, x0, y0, x1, y1 inclusive,
 * y up; -1 for no such display.  VDisplay_Changed's, for whoever shows it. */
int ros_vdisplay_take(uint32_t handle, int32_t box[4]);
/* Called, if set, whenever a display changes (only to note it: the VDU may
 * be in the middle of drawing), and at each VSync after the displays'
 * flashing, from background work with the real display live */
extern void (*ros_vdisplay_changed_hook)(void);
/* The live context saved whole, and put back (one level): around drawing
 * that background work does on the real display */
void ros_vdu_snapshot_take(void);
void ros_vdu_snapshot_restore(void);
extern void (*ros_vdisplay_vsync_hook)(void);

#endif
