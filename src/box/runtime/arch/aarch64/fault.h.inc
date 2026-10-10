/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fault.h.inc -- what runtime/fault.c's shared half needs of AArch64 before
 * its fault record (included where it stands): the registers as a signal
 * has them, with the ESR the kernel records for the fault and FPSR. */

typedef struct {
    uint64_t x[31], sp, pc, pstate;     /* the context's */
    uint64_t esr;                       /* its esr_context's, or 0 if it had none */
    uint32_t fpsr;                      /* its fpsimd_context's */
} fault_regs;

/* The fault's native pc and sp, for the report and for delivery. Also
 * whether a fault in code below 4 GB was on the program's own stack, whose
 * sp delivery takes. A64X32 code runs on an arena stack. BASIC's AArch64
 * code does too, as a nested entry (capp.c's ros_capp_call). */
#define FAULT_NATIVE_PC(f)  ((f)->g.pc)
#define FAULT_SP(f)         ((f)->g.sp)
#define FAULT_TO_PROGRAM(f) ((f)->g.sp < ARENA_TOP)
