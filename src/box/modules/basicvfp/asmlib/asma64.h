/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asma64.h: the AArch64 encoder's entry points. See asm.h for the interface. */
#ifndef BASICASM_ASMA64_H
#define BASICASM_ASMA64_H

#include "asm.h"

/* Says whether the text is just a condition suffix. The BBC BASIC driver
 * also uses this. */
int asma64_is_cond(const char *b, const char *e, int *cond);

#endif
