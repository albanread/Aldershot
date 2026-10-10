/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* vfpsupport.h: VFPSupport, reimplemented (vfpsupport.c).
 *
 * Its SWIs work at register level, as its callers use them. The thunks are
 * in vfpsupport.c. This header is for the ROM's list of modules.
 */
#ifndef ROSGD_VFPSUPPORT_H
#define ROSGD_VFPSUPPORT_H

extern struct ros_module vfpsupport_module;

#endif
