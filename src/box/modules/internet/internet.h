/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* internet.h -- the Internet module, over Linux's sockets (README.md).
 *
 * Its interface for C is the typed Socket_* calls in the generated api.h;
 * this header is for the ROM's list of modules.
 */
#ifndef ROSGD_INTERNET_H
#define ROSGD_INTERNET_H

/* The module, for the ROM's list of native modules. */
extern struct ros_module internet_module;

#include <stdint.h>

/* For modules layered on a socket (AcornSSL): the Linux descriptor behind a
 * RISC OS socket number, or -1; and the Internet event re-armed after
 * reading from it directly. */
int ros_internet_fd(uint32_t s);
void ros_internet_consumed(uint32_t s);

#endif
