/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* callback.h -- the kernel's old-style CallBack (callback.c). */
#ifndef ROSGD_CALLBACK_H
#define ROSGD_CALLBACK_H

#include "rosgd/cpu.h"

/* On the way out of the outermost SWI: if OS_SetCallBack was called and the
 * CallBack handler is not the default, dump the caller's registers into
 * the handler's buffer and run it.  If it goes back to user mode with a
 * block (ros_user_return), s has that block's registers. */
void ros_callback_run(struct ros_cpu *s);

#endif
