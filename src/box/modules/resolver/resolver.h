/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* resolver.h -- the Resolver, over the host's name lookup (README.md).
 *
 * Its interface for C is the typed Resolver_* calls in the generated api.h;
 * this header is for the ROM's list of modules.
 */
#ifndef ROSGD_RESOLVER_H
#define ROSGD_RESOLVER_H

/* The module, for the ROM's list of native modules. */
extern struct ros_module resolver_module;

#endif
