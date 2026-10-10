/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* faultblock.c.inc -- the AArch64 register block of a fault (fault.h's
 * table), for runtime/fault.c's landing, which includes it where it
 * stands. It also holds the report's machine line. */

/* The register block, R0-R15 then the PSR, taken from the A64 registers.
 * R0-R12 are x0-x12, R13 is sp, R14 is x30 and R15 is the pc. The PSR is
 * the mode the code was entered in, plus N Z C V, which PSTATE keeps in the
 * same bits as the ARM (31-28). x13-x29 are left out, but the report has
 * x18 (the static base) and x29. When the runtime faults in a SWI, the
 * block is the program's registers as it called the OS, as on x86-64. */
static const char *const dump_name[16] = { "x0", "x1", "x2",  "x3",  "x4",  "x5", "x6",  "x7",
                                           "x8", "x9", "x10", "x11", "x12", "sp", "x30", "pc" };

static void block(const struct fault *f, uint32_t r[17])
{
    memset(r, 0, 17 * sizeof r[0]);
    if (f->where == X32) {
        for (unsigned i = 0; i < 13; i++)
            r[i] = (uint32_t)f->g.x[i];
        r[13] = (uint32_t)f->g.sp;
        r[14] = (uint32_t)f->g.x[30];
        r[15] = f->pc;
        r[16] = ros_capp_mode() | ((uint32_t)f->g.pstate & 0xF0000000u);   /* N Z C V */
        return;
    }
    if (f->in_gate) {
        r[13] = f->gate_sp;
        r[14] = r[15] = f->gate_ret;
    } else if (ros_env_foreground_regs(r)) {
        r[15] = r[14];
    }
    r[16] = ROS_MODE_SVC;
}

/* The report's last line for code below 4 GB: the PSR, and what the block
 * leaves out: x18 (the static base), x29, FPCR, FPSR and the ESR */
#define SAY_MACHINE(f, r)                                                                   \
    say("  PSR &%08X (%s), x18 &%llX, x29 &%llX, fpcr &%X, fpsr &%X, esr &%llX", r[16],     \
        mode_name(r[16]), (unsigned long long)(f)->g.x[18], (unsigned long long)(f)->g.x[29], \
        (f)->fpc, (f)->g.fpsr, (unsigned long long)(f)->g.esr)
