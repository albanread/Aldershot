/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* sshd.h -- SSHD, a native module: OpenSSH's sshd in the box, its sessions
 * RISC OS command lines (sshd.c, session.c, relay.c).
 */
#ifndef ROSGD_SSHD_H
#define ROSGD_SSHD_H

#include <stddef.h>

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module sshd_module;

/* Where /init listens for riscos-cli */
#define SSHD_SOCKET "/run/rosgd-cli.sock"

/* sshd, as *SSHD starts it. The options are its port, where its host key is
 * kept (made there the first time), and the file of keys that may log in */
struct sshd_options {
    int port;
    const char *keydir;
    const char *authorized;
};
os_error *sshd_start(const struct sshd_options *o, char *report, size_t size);
os_error *sshd_stop(void);
int sshd_running(void);                 /* its port, or 0 */

/* The command lines (session.c). This listens at path, as the box does at
 * SSHD_SOCKET. It returns 0 or -errno */
int sshd_listen(const char *path);
void sshd_unlisten(void);
int sshd_session_count(void);           /* open now */
int sshd_sessions_ever(void);

/* riscos-cli (relay.c): /init run as root's login shell */
int sshd_relay(int argc, char **argv);

#endif
