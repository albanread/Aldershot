/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* deskclock.h declares DeskClock, an analogue clock on the icon bar. */
#ifndef ROSGD_DESKCLOCK_H
#define ROSGD_DESKCLOCK_H

#include "rosgd/module.h"

extern struct ros_module deskclock_module;

/* Whether the desktop starts DeskClock (on Service_StartWimp). The default,
 * -1, follows the kernel command line. The run scripts give rosgd.deskclock
 * to a box that is started in a window, and so DeskClock is used there. The
 * probes' boxes keep RISC OS's icon bar as 5.30 has it. A value of 0 or 1
 * overrides the command line (the self-test does this). */
extern int deskclock_autostart;

#endif
