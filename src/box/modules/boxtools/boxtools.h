/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* boxtools.h: BoxTools, a native module for developing in the box.
 * It provides *CC (clang, or tcc when hosted), *RunBox, *RosBas, *Mojo and
 * *RosAsm (boxtools.c).
 */
#ifndef ROSGD_BOXTOOLS_H
#define ROSGD_BOXTOOLS_H

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module boxtools_module;

/* The C spelling of a source's program name. The last "c" directory in
 * the path is left out (c.hello -> hello, $.c.proj.c.x -> $.c.proj.x).
 * Or a "/c" suffix is taken off (hello/c -> hello). Returns 0 if the path
 * has neither. */
int boxtools_program_name(const char *src, char *out, size_t max);

#endif
