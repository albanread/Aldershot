/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* kcheck.h: the checker that Worker_LoadKernel runs over an application
 * kernel's code (modules/worker; README.md there, "Kernels").
 *
 * It is plain C over bytes and uses nothing from the runtime, so a host
 * test can run it too. The checks for both machines are built on both
 * boxes.
 */
#ifndef ROSGD_WORKER_KCHECK_H
#define ROSGD_WORKER_KCHECK_H

#include <stddef.h>
#include <stdint.h>

#define WK_EM_X86_64  62u
#define WK_EM_AARCH64 183u

/* Check code[0, len), which will run at address base, for machine (one of
 * the WK_EM_* values). Every PC-relative reference must land in [lo, hi),
 * the kernel's whole image (code and read-only data). These references are
 * a branch's target, a literal load, adr and adrp, and x86-64's [rip + d].
 * Jumps and calls must land in [base, base + len), the code. The result is
 * 0 if the code passes. Otherwise it is 1 plus the offset of the first
 * instruction refused. The reason for the refusal is written to
 * why[0, size) as a short sentence with no full stop. */
uint32_t wk_check(unsigned machine, const uint8_t *code, uint32_t len, uint32_t base, uint32_t lo,
                  uint32_t hi, char *why, size_t size);

#endif
