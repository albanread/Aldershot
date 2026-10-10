/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* urlfetcher.h -- URL_Fetcher (urlfetcher.c).
 *
 * Its SWIs are register-level (api/defs/urlfetcher.toml); this header is for
 * the ROM's list of modules.
 */
#ifndef ROSGD_URLFETCHER_H
#define ROSGD_URLFETCHER_H

/* The module, for the ROM's list of native modules. */
extern struct ros_module urlfetcher_module;

#endif
