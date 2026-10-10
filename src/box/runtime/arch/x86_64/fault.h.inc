/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fault.h.inc -- what runtime/fault.c's shared half needs of x86-64 before
 * its fault record (included where it stands): the registers as a signal
 * has them, and the trap numbers and error bits the classifier reads. */

typedef greg_t fault_regs[NGREG];

/* x86's trap numbers (the context's REG_TRAPNO), and a page fault's error
 * code bit for an instruction fetch (REG_ERR) */
enum { TRAP_DE = 0, TRAP_DB = 1, TRAP_BP = 3, TRAP_UD = 6, TRAP_GP = 13, TRAP_PF = 14,
       TRAP_MF = 16, TRAP_AC = 17, TRAP_XM = 19 };
#define PF_FETCH 0x10u

/* The fault's native pc and sp, for the report and for delivery. Also
 * whether a fault in x32 code was on the program's own stack, whose sp
 * delivery takes. On x86-64 it always was, because x32 code runs on arena
 * stacks. */
#define FAULT_NATIVE_PC(f) ((uint64_t)(f)->g[REG_RIP])
#define FAULT_SP(f)        ((f)->g[REG_RSP])
#define FAULT_TO_PROGRAM(f) 1
