/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* waylandwin.h -- WaylandWindows: Linux programs' windows as Wimp windows
 * (waylandwin.c, browser.c). */
#ifndef ROSGD_WAYLANDWIN_H
#define ROSGD_WAYLANDWIN_H

#include "rosgd/module.h"

extern struct ros_module waylandwin_module;

/* Whether the desktop starts it (Service_StartWimp): -1, the default, as
 * the kernel command line says (rosgd.display=compositor); 0 or 1
 * decides, for the self-test. */
extern int waylandwin_autostart;

#endif
