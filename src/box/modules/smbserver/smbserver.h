/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* smbserver.h -- SMBServer, a native module: the box's directories shared
 * over SMB3 by ksmbd (smbserver.c).
 */
#ifndef ROSGD_SMBSERVER_H
#define ROSGD_SMBSERVER_H

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module smbserver_module;

/* *Share, *UnShare and *SMBUser, from C. A name of NULL means the directory's
 * own. A password of NULL removes the user. A guest share that is not read
 * only is refused unless guest_write is set (*Share's -write) */
os_error *smb_share(const char *dir, const char *name, int readonly, int guest, int guest_write);
os_error *smb_unshare(const char *name);
os_error *smb_user(const char *user, const char *password);
int smb_running(void);

#endif
