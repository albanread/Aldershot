/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* pty.h -- the PTY module (pty.c): Linux programs on pseudo-terminals, and
 * a VT100 terminal over the VDU drivers for them (term.c).
 */
#ifndef ROSGD_PTY_H
#define ROSGD_PTY_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module pty_module;

/* PTY's errors: &C00C0 + n, in its SWI chunk, as T0Demo's are */
#define PTY_ERR_BAD_HANDLE 0xC00C0u
#define PTY_ERR_CANNOT_RUN 0xC00C1u
#define PTY_ERR_NO_PTY 0xC00C2u

/* A program on a pseudo-terminal: PTY_Open and the rest, from C */
struct pty;
os_error *pty_open(const char *cmdline, int cols, int rows, struct pty **out);
long pty_read(struct pty *p, void *buf, size_t n);     /* 0 means nothing yet (or ever). It never blocks */
long pty_write(struct pty *p, const void *buf, size_t n);
int pty_ended(struct pty *p, int *status);             /* 1 once it has ended and been read dry */
int pty_fd(struct pty *p);                             /* the master side, to poll */
int pty_close(struct pty *p);                          /* SIGHUP, wait, free: the exit status */

/* Runs a command line on a pseudo-terminal the size of the text window, and
 * is its terminal until it ends. A VT100's escape sequences are drawn with
 * VDU codes, and the keyboard is sent to the program. *status is its exit
 * status. */
os_error *pty_terminal(const char *cmdline, int *status);

/* The same, leaving out each line of its output that contains drop (a
 * warning a tool always gives, say). A drop of NULL leaves out nothing. */
os_error *pty_terminal_dropping(const char *cmdline, const char *drop, int *status);

/* The terminal alone, for the self-test. It begins at the cursor in the text
 * window, is fed what a program would write, and ends. Replies (to a cursor
 * position request, say) are dropped. */
void pty_term_replay(const void *bytes, size_t n);

#endif
