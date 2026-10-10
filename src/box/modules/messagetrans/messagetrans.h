/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* messagetrans.h -- MessageTrans, reimplemented (messagetrans.c).
 *
 * Its SWIs are register-level, as its callers use them: the thunks are
 * in messagetrans.c.  This header is for the ROM's list of modules.
 */
#ifndef ROSGD_MESSAGETRANS_H
#define ROSGD_MESSAGETRANS_H

extern struct ros_module messagetrans_module;

#endif
