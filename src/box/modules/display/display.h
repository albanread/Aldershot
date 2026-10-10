/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* display.h: DisplayManager, the screen mode chooser (README.md). */
#ifndef ROSGD_DISPLAY_H
#define ROSGD_DISPLAY_H

#include <stddef.h>

#include "rosgd/module.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module display_module;

/* The self-test's entry point. It builds the task's menu lists and follows
 * every resolution choice through to a mode string. It returns 0, or -1
 * with the reason in why. */
int ros_display_check_menus(char *why, size_t size);

#endif
