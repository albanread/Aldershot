/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* acornssl.h -- AcornSSL, over OpenSSL (acornssl.c).
 *
 * Its SWIs are register-level (api/defs/acornssl.toml); this header is for
 * the ROM's list of modules.
 */
#ifndef ROSGD_ACORNSSL_H
#define ROSGD_ACORNSSL_H

/* The module, for the ROM's list of native modules. */
extern struct ros_module acornssl_module;

#endif
