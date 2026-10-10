/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* fpemulator.h: FPEmulator, a native stub (fpemulator.c). It has the name,
 * version and SWIs of RISC OS 5.30's FPEmulator and emulates nothing (#114). */
#ifndef ROSGD_FPEMULATOR_H
#define ROSGD_FPEMULATOR_H

extern struct ros_module fpemulator_module;

/* FPEmulator_Version's R0 and the version in the help string. They are
 * those of 5.30's ROM FPEmulator: 4.39 (30 Mar 2024), FPASC 1.13CELM. */
#define FPEMULATOR_VERSION 439u
/* FPEmulator_ContextLength's R0, the size of an FPA context in bytes. It is 5.30's value. */
#define FPEMULATOR_CONTEXT_LENGTH 136u

#endif
