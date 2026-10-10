/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* smooth.h: Smooth, the desktop's anti-aliasing switch (modules/smooth).
 * Its interface is its *commands and its choices file. This header is for
 * the ROM's list of modules, and for the self-test.
 */
#ifndef ROSGD_SMOOTH_H
#define ROSGD_SMOOTH_H

extern struct ros_module smooth_module;

/* For the self-test: take this as the current Wimp task's name, in place of
 * the Wimp's. NULL means there is no task. smooth_test_task_clear() goes back
 * to the Wimp's. */
void smooth_test_task(const char *name);
void smooth_test_task_clear(void);

#endif
