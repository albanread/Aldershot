/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* armrun.h: the ARM container's engine, as C.
 *
 * The engine is dynarmic (deps/build-dynarmic.sh). It translates A32 code to
 * the host's instruction set. That is AArch64 in the Apple Silicon box and
 * the Studio's hosted build, and x86-64 in the Intel box. This header is the
 * whole face of the engine to the runtime. Nothing outside runtime/armrun
 * includes C++.
 *
 * There is one engine per ARM task. Guest memory is the arena. A guest
 * address a is the host's base + a ("fastmem"). Translated loads and stores
 * therefore go straight to the arena. Only an access that faults comes back
 * here, to the read and write hooks. In the box the runtime's signal handler
 * offers such a fault to the engine first (ros_fault_jit, fault.h;
 * armrun_handle_fault below).
 *
 * The engine models user mode only. The bridge looks after the mode and the
 * banked registers of the RISC OS handlers that the engine is asked to
 * enter (ABI.md §3).
 */
#ifndef ROSGD_ARMRUN_H
#define ROSGD_ARMRUN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct armrun;

/* What armrun_run's guest raised (dynarmic's A32::Exception, in order) */
enum armrun_exception {
    ARMRUN_UNDEFINED,           /* an unallocated encoding. FPA's too, if the coproc hook
                                   is unset so that CP1 and CP2 are not installed */
    ARMRUN_UNPREDICTABLE,
    ARMRUN_DECODE_ERROR,
    ARMRUN_SEV, ARMRUN_SEVL, ARMRUN_WFI, ARMRUN_WFE, ARMRUN_YIELD,
    ARMRUN_BREAKPOINT,          /* BKPT */
    ARMRUN_PLD, ARMRUN_PLDW, ARMRUN_PLI,
    ARMRUN_NO_EXECUTE,          /* code at an address the code hook refused */
    ARMRUN_INTERPRET,           /* one the engine does not translate, which the bridge
                                   executes: LDM/STM ^, LDM ^ with pc, CPS, RFE, SRS
                                   (patches/0003). R15 is already past it */
};

/* Why armrun_run returned (bits; 0: its ticks ran out) */
#define ARMRUN_HALTED   0x01000000u     /* armrun_halt */
#define ARMRUN_STEPPED  0x00000001u     /* armrun_step: one instruction done */
#define ARMRUN_ABORTED  0x00000004u     /* armrun_abort: R15 the access's instruction */
#define ARMRUN_STOP_MASK 0xFF000005u    /* ours and the step; dynarmic's own
                                           reasons (cache invalidation) are internal */

struct armrun_hooks {
    void *ctx;
    /* An SVC: R15 already past it, as the kernel's SWI entry sees it.  The
     * registers are the engine's (armrun_regs); a hook may change them,
     * and may call armrun_halt to stop at the end of the block. */
    void (*svc)(void *ctx, struct armrun *a, uint32_t number);
    /* An exception at pc (enum armrun_exception) */
    void (*exception)(void *ctx, struct armrun *a, uint32_t pc, int kind);
    /* A guest access that did not go straight to memory: one that faulted,
     * or one the engine makes itself (code fetches go to fetch).  size is
     * 1, 2, 4 or 8. */
    uint64_t (*read)(void *ctx, struct armrun *a, uint32_t addr, unsigned size);
    void (*write)(void *ctx, struct armrun *a, uint32_t addr, unsigned size, uint64_t value);
    /* An instruction word to translate, *word set and 1; or 0: none there
     * (the block raises ARMRUN_NO_EXECUTE, a prefetch abort) */
    int (*fetch)(void *ctx, struct armrun *a, uint32_t addr, uint32_t *word);
    /* When set, CP1 and CP2 (the FPA) are installed. Every instruction on
     * them, as it is translated, becomes a direct call to this. The
     * instruction is rebuilt from its fields (condition 1110, an MCR/MRC's
     * transfer register 0). arg is the word that an MCR sends, or the address
     * of an LDC/STC. An MRC's result is the return value (into Rt, or into
     * the flags when Rt = pc). When unset, those instructions are undefined. */
    uint64_t (*coproc)(void *ctx, struct armrun *a, uint32_t word, uint32_t arg);
};

/* 1 (the default): a data abort stops the block at the access, so the
 * error's PC is exact (dynarmic's check_halt_on_memory_access; on x86-64 it
 * costs the get/set elimination pass). 0 is only for measuring what that
 * costs (rosgd.armnohalt). */
extern int armrun_halt_on_access;

/* A new engine over guest memory at base (the arena's origin: 0 in the box),
 * with a code cache of cache_bytes (0: 16 MB; at least 8 MB) */
struct armrun *armrun_create(uintptr_t base, size_t cache_bytes, const struct armrun_hooks *hooks);
void armrun_destroy(struct armrun *a);

/* Run from R15 for at most ticks guest instructions (about), or until
 * armrun_halt. Returns the reason, as ARMRUN_* bits. */
uint32_t armrun_run(struct armrun *a, uint64_t ticks);
/* What armrun_run's budget had left when it returned */
uint64_t armrun_ticks_left(const struct armrun *a);
/* One instruction */
uint32_t armrun_step(struct armrun *a);
/* Stop at the end of the current block: from a hook, or another thread */
void armrun_halt(struct armrun *a);
/* From a memory hook: stop right after the access, R15 then the address of
 * the instruction that made it (a data abort) */
void armrun_abort(struct armrun *a);

/* Code changed at [addr, addr + len): its translations go (OS_SynchroniseCodeAreas) */
void armrun_invalidate(struct armrun *a, uint32_t addr, uint32_t len);
/* Every translation goes (a new image in the slot) */
void armrun_clear(struct armrun *a);

/* The guest's registers: r0-r15, the CPSR, VFP/NEON's 64 words (s0-s63,
 * d0-d31 as pairs) and the FPSCR */
uint32_t *armrun_regs(struct armrun *a);
uint32_t armrun_cpsr(const struct armrun *a);
void armrun_set_cpsr(struct armrun *a, uint32_t cpsr);
uint32_t *armrun_ext(struct armrun *a);
uint32_t armrun_fpscr(const struct armrun *a);
void armrun_set_fpscr(struct armrun *a, uint32_t fpscr);

/* The box's signal handler's question (ros_fault_jit): 1 when the fault is
 * a guest access in translated code, now on its way to the read or write
 * hook; 0 when it is the handler's. Hosted, faults are dynarmic's own. */
int armrun_handle_fault(int sig, void *siginfo, void *context);

#ifdef __cplusplus
}
#endif

#endif
