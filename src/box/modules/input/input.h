/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* input.h -- Input, the keyboard and pointer driver (README.md).
 *
 * Its interface is the vectors it calls and answers, KeyV and PointerV.
 * This header is for the ROM's list of modules, and for what delivers to it.
 */
#ifndef ROSGD_INPUT_H
#define ROSGD_INPUT_H

#include "rosgd/platform.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module input_module;

/* One event, in RISC OS's terms, at a safe point: from the platform's
 * devices, or from a test. */
void input_deliver(const struct ros_input_event *ev);

/* How many input devices it found. */
int input_device_count(void);

#endif
