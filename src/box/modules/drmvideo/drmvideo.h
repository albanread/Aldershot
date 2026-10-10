/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* drmvideo.h -- DRMVideo, the GraphicsV driver (README.md).
 *
 * Its interface is GraphicsV itself, the vector, which the VDU drivers
 * call.  This header is for the ROM's list of modules, and for the parts
 * of the box that draw before any VDU driver exists.
 */
#ifndef ROSGD_DRMVIDEO_H
#define ROSGD_DRMVIDEO_H

#include "rosgd/platform.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module drmvideo_module;

/* The screen as the current mode made it, or NULL with no display. */
const struct ros_display *drmvideo_display(void);

#endif
