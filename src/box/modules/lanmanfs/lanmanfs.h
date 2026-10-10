/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* lanmanfs.h -- LanManFS, a native module: SMB2/3 shares through libsmb2,
 * each a HostFS disc (lanmanfs.c).
 */
#ifndef ROSGD_LANMANFS_H
#define ROSGD_LANMANFS_H

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module lanmanfs_module;

/* *LMConnect and *LMDisconnect, from C. A user of NULL means *LMLogon's, or a
 * guest's if it gave none */
os_error *lanman_connect(const char *name, const char *server, const char *share, const char *user,
                         const char *password);
os_error *lanman_disconnect(const char *name);
int lanman_connection_count(void);

#endif
