/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* faultblock.c.inc -- the x86-64 register block of a fault (fault.h's
 * table), for runtime/fault.c's landing, which includes it where it
 * stands. It also holds the report's machine line. */

/* The register block (fault.h): the x86-64 registers as R0-R15, then the
 * PSR. When the runtime faults, the block is the program's registers as it
 * called the OS. */
static const int dump_reg[16] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8,  REG_R9,
                                  REG_RAX, REG_RBX, REG_R12, REG_R13, REG_R14, REG_RBP,
                                  REG_R10, REG_RSP, REG_R15, REG_RIP };
static const char *const dump_name[16] = { "rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                           "r12", "r13", "r14", "rbp", "r10", "rsp", "r15", "rip" };

static void block(const struct fault *f, uint32_t r[17])
{
    memset(r, 0, 17 * sizeof r[0]);
    if (f->where == X32) {
        for (unsigned i = 0; i < 16; i++)
            r[i] = (uint32_t)f->g[dump_reg[i]];
        r[15] = f->pc;
        uint64_t fl = (uint64_t)f->g[REG_EFL];
        r[16] = ros_capp_mode() | (fl & 0x80 ? 1u << 31 : 0) | (fl & 0x40 ? 1u << 30 : 0) |
                (fl & 0x01 ? 1u << 29 : 0) | (fl & 0x800 ? 1u << 28 : 0);   /* N Z C V */
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

/* The report's last line for x32 code: the PSR, and what the block leaves
 * out: r11, the floating point control and the static base */
#define SAY_MACHINE(f, r)                                                                   \
    say("  PSR &%08X (%s), r11 &%08X, mxcsr &%04X, x87 cw &%04X, gs &%llX", r[16],           \
        mode_name(r[16]), (unsigned)(f)->g[REG_R11], (f)->fpc, (f)->fpc2,                   \
        (unsigned long long)gs_base())
