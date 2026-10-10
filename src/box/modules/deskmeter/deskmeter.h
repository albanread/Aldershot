/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* deskmeter.h declares DeskMeter, which graphs how busy the Wimp is on the icon bar. */
#ifndef ROSGD_DESKMETER_H
#define ROSGD_DESKMETER_H

#include "rosgd/module.h"

extern struct ros_module deskmeter_module;

/* Whether the desktop starts DeskMeter (on Service_StartWimp). The default,
 * -1, follows the kernel command line. The run scripts give rosgd.deskmeter
 * to a box that is started in a window, and so DeskMeter is used there. The
 * probes' boxes keep RISC OS's icon bar as 5.30 has it. A value of 0 or 1
 * overrides the command line (the self-test does this). */
extern int deskmeter_autostart;

#endif
