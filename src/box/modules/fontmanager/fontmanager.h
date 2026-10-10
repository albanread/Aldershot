/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* fontmanager.h: the Font Manager, reimplemented in fontmanager.c and the
 * other files here, and ROMFonts, which points Font$Path at the ROM's
 * fonts.
 *
 * The interface of both modules is their SWIs and variables.  This header
 * only gives the ROM's list of modules what it needs.
 */
#ifndef ROSGD_FONTMANAGER_H
#define ROSGD_FONTMANAGER_H

extern struct ros_module fontmanager_module, romfonts_module;

/* If set, this is called at each Font_Paint.  A non-zero result makes the
 * paint blended, as if bit 11 of R2 were set.  The Smooth module
 * (modules/smooth) uses it as its switch. */
extern int (*fm_paint_blend_hook)(void);

#endif
