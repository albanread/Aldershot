/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fpa.h: the FPA, native, for the ARM container (fpa.c) */
#ifndef ROSGD_ARMRUN_FPA_H
#define ROSGD_ARMRUN_FPA_H

#include <stdint.h>

struct ros_fp;

/* One FPA instruction (cp1 or cp2), word rebuilt from what the engine
 * decoded (its condition and transfer register aside), on fp: arg the word
 * an MCR sends, or an LDC/STC's address (guest memory at base + address).
 * An MRC's word is the result. *error is 0, or the error to raise: a trap
 * that the FPSR enables (&80000200 + n), or &80000000 for no FPA instruction. */
uint64_t fpa_execute(struct ros_fp *fp, uintptr_t base, uint32_t word, uint32_t arg, uint32_t *error);

/* FPEmulator's text for those errors */
const char *fpa_error_text(uint32_t error);

#endif
