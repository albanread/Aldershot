/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* dhcp.h -- DHCP, over Linux's kernel DHCP (dhcp.c).
 *
 * Its interface for C is the typed DHCP_* calls in the generated api.h;
 * this header is for the ROM's list of modules.
 */
#ifndef ROSGD_DHCP_H
#define ROSGD_DHCP_H

/* The module, for the ROM's list of native modules. */
extern struct ros_module dhcp_module;

#endif
