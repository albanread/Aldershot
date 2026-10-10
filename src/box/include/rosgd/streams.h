/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* streams.h -- where characters go and come from (runtime/streams.c):
 * *Spool, *Exec, OS_CLI's redirection, OS_Byte 3, 198 and 199. */
#ifndef ROSGD_STREAMS_H
#define ROSGD_STREAMS_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

/* WrchV's and RdchV's default owners */
os_error *ros_wrch_default(uint8_t ch);
os_error *ros_rdch_default(uint8_t *ch, int *escape);

/* OS_Byte 3, 198 and 199: 1 if it was one of them */
int ros_streams_byte(struct ros_cpu *s);

/* A second command line, such as an SSH session's (modules/sshd), has its
 * own task, and takes that task's character streams. Each hook is tried
 * first and says whether it took the call. idle is called while the box
 * has nothing of its own to do, such as the console waiting for a key or
 * the Wimp polling with no event for any task, so other command lines may
 * run. */
struct ros_stream_hooks {
    int (*wrch)(uint8_t ch);                    /* 1 taken */
    int (*rdch)(uint8_t *ch, int *escape);      /* 1 taken */
    int (*inkey)(int timeout_cs);               /* -2 not taken; -1 none in time; the key */
    void (*idle)(void);
    int (*owns)(void);                          /* 1 if the running task's streams are its */
};
extern const struct ros_stream_hooks *ros_stream_hooks;

/* Whether the running task's characters are a second command line's (an
 * SSH session's task, or a Wimp task its command started): what it writes
 * goes to that terminal, never to the desktop's command window (#164) */
int ros_streams_own(void);

/* The console waits: background work, and the hooks' idle, meanwhile */
void ros_streams_idle(void);

/* OS_CLI's redirection: mode &40 "<", &80 ">", &C0 ">>", the name at an
 * arena address; and undoing all of it (the kernel's OscliTidy) */
os_error *ros_redirect(uint32_t mode, uint32_t name);
void ros_redirect_tidy(void);

#endif
