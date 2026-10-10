/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
// armrun.cpp: the ARM container's engine. It is dynarmic's A32 JIT behind
// armrun.h's C face.
//
// The configuration is ARMv8 AArch32 (a superset of every RISC OS binary's
// architecture) and little-endian. Guest memory is the arena, through
// fastmem. There is one exclusive monitor per engine, because the box runs
// one ARM task's code on one thread at a time (the baton). No coprocessors
// are installed unless the embedder sets armrun_hooks::coproc, which installs
// the FPA on CP1 and CP2. Without it an FPA instruction is an undefined
// instruction, for the FPEmulator module to handle.

#include "armrun.h"

#include <csignal>
#include <cstring>
#include <memory>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

using namespace Dynarmic;

namespace {

constexpr size_t DEFAULT_CACHE = 16u << 20;
constexpr size_t MIN_CACHE = 8u << 20;

}  // namespace

struct armrun;

namespace {

// An FPA instruction as the coprocessor compiled it: the call's user_arg
struct CoprocOp {
    armrun *a;
    std::uint32_t word;
};

// CP1 or CP2: every instruction is the embedder's (armrun_hooks::coproc),
// rebuilt from the fields the translator decoded
class Fpa final : public A32::Coprocessor {
public:
    Fpa(armrun *a, unsigned cp) : a_(a), cp_(cp) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned opc1, A32::CoprocReg CRd, A32::CoprocReg CRn,
                                                     A32::CoprocReg CRm, unsigned opc2) override
    {
        return call(0xEE000000u | opc1 << 20 | (unsigned)CRn << 16 | (unsigned)CRd << 12 | opc2 << 5 | (unsigned)CRm);
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                               unsigned opc2) override
    {
        return *call(0xEE000010u | opc1 << 21 | (unsigned)CRn << 16 | opc2 << 5 | (unsigned)CRm);
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return {}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                              unsigned opc2) override
    {
        return *call(0xEE100010u | opc1 << 21 | (unsigned)CRn << 16 | opc2 << 5 | (unsigned)CRm);
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return {}; }
    std::optional<Callback> CompileLoadWords(bool, bool long_transfer, A32::CoprocReg CRd,
                                             std::optional<std::uint8_t>) override
    {
        return call(0xEC100000u | (long_transfer ? 1u << 22 : 0) | (unsigned)CRd << 12);
    }
    std::optional<Callback> CompileStoreWords(bool, bool long_transfer, A32::CoprocReg CRd,
                                              std::optional<std::uint8_t>) override
    {
        return call(0xEC000000u | (long_transfer ? 1u << 22 : 0) | (unsigned)CRd << 12);
    }

    std::vector<std::unique_ptr<CoprocOp>> ops;     // the compiled calls' arguments

private:
    std::optional<Callback> call(std::uint32_t word);
    armrun *a_;
    unsigned cp_;
};

}  // namespace

struct armrun final : A32::UserCallbacks {
    armrun_hooks hooks;
    std::shared_ptr<Fpa> cp1, cp2;
    std::unique_ptr<ExclusiveMonitor> monitor;
    std::unique_ptr<A32::Jit> jit;
    uint64_t ticks = 0;

    std::uint8_t MemoryRead8(A32::VAddr a) override { return (std::uint8_t)hooks.read(hooks.ctx, this, a, 1); }
    std::uint16_t MemoryRead16(A32::VAddr a) override { return (std::uint16_t)hooks.read(hooks.ctx, this, a, 2); }
    std::uint32_t MemoryRead32(A32::VAddr a) override { return (std::uint32_t)hooks.read(hooks.ctx, this, a, 4); }
    std::uint64_t MemoryRead64(A32::VAddr a) override { return hooks.read(hooks.ctx, this, a, 8); }
    void MemoryWrite8(A32::VAddr a, std::uint8_t v) override { hooks.write(hooks.ctx, this, a, 1, v); }
    void MemoryWrite16(A32::VAddr a, std::uint16_t v) override { hooks.write(hooks.ctx, this, a, 2, v); }
    void MemoryWrite32(A32::VAddr a, std::uint32_t v) override { hooks.write(hooks.ctx, this, a, 4, v); }
    void MemoryWrite64(A32::VAddr a, std::uint64_t v) override { hooks.write(hooks.ctx, this, a, 8, v); }

    // STREX: the monitor has checked the reservation; the store is the
    // compare-and-store it asks for, through the same hooks
    template<typename T>
    bool exclusive(A32::VAddr a, T v, T expected)
    {
        if ((T)hooks.read(hooks.ctx, this, a, sizeof(T)) != expected)
            return false;
        hooks.write(hooks.ctx, this, a, sizeof(T), v);
        return true;
    }
    bool MemoryWriteExclusive8(A32::VAddr a, std::uint8_t v, std::uint8_t e) override { return exclusive(a, v, e); }
    bool MemoryWriteExclusive16(A32::VAddr a, std::uint16_t v, std::uint16_t e) override { return exclusive(a, v, e); }
    bool MemoryWriteExclusive32(A32::VAddr a, std::uint32_t v, std::uint32_t e) override { return exclusive(a, v, e); }
    bool MemoryWriteExclusive64(A32::VAddr a, std::uint64_t v, std::uint64_t e) override { return exclusive(a, v, e); }

    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr a) override
    {
        std::uint32_t w;
        if (!hooks.fetch(hooks.ctx, this, a, &w))
            return std::nullopt;
        return w;
    }

    void InterpreterFallback(A32::VAddr pc, size_t) override
    {
        // dynarmic has no interpreter: an instruction it cannot translate
        hooks.exception(hooks.ctx, this, pc, ARMRUN_DECODE_ERROR);
    }
    void CallSVC(std::uint32_t n) override { hooks.svc(hooks.ctx, this, n); }
    void ExceptionRaised(A32::VAddr pc, A32::Exception e) override
    {
        hooks.exception(hooks.ctx, this, pc, (int)e);
    }

    void AddTicks(std::uint64_t n) override { ticks = n >= ticks ? 0 : ticks - n; }
    std::uint64_t GetTicksRemaining() override { return ticks; }
};

namespace {

std::uint64_t coproc_trampoline(void *user, std::uint32_t arg0, std::uint32_t)
{
    auto *op = static_cast<CoprocOp *>(user);
    return op->a->hooks.coproc(op->a->hooks.ctx, op->a, op->word, arg0);
}

std::optional<A32::Coprocessor::Callback> Fpa::call(std::uint32_t word)
{
    ops.push_back(std::make_unique<CoprocOp>(CoprocOp{a_, word | cp_ << 8}));
    return Callback{coproc_trampoline, ops.back().get()};
}

}  // namespace

static_assert((int)A32::Exception::NoExecuteFault == ARMRUN_NO_EXECUTE, "armrun_exception follows A32::Exception");
static_assert((int)A32::Exception::InterpretRequired == ARMRUN_INTERPRET, "armrun_exception follows A32::Exception");
static_assert((std::uint32_t)HaltReason::UserDefined1 == ARMRUN_HALTED, "ARMRUN_HALTED is UserDefined1");
static_assert((std::uint32_t)HaltReason::Step == ARMRUN_STEPPED, "ARMRUN_STEPPED is Step");
static_assert((std::uint32_t)HaltReason::MemoryAbort == ARMRUN_ABORTED, "ARMRUN_ABORTED is MemoryAbort");

extern "C" {

int armrun_halt_on_access = 1;

struct armrun *armrun_create(uintptr_t base, size_t cache_bytes, const struct armrun_hooks *hooks)
{
    auto a = std::make_unique<armrun>();
    a->hooks = *hooks;
    a->monitor = std::make_unique<ExclusiveMonitor>(1);
    A32::UserConfig c;
    c.callbacks = a.get();
    c.processor_id = 0;
    c.global_monitor = a->monitor.get();
    c.arch_version = A32::ArchVersion::v8;
    c.fastmem_pointer = base;
    c.always_little_endian = true;
    // A guest access that the hooks refuse stops the guest at that
    // instruction (armrun_abort), with R15 at its address. The check is
    // emitted only on the paths that a fastmem fault falls back to, so
    // ordinary accesses pay nothing. On x86-64 it also turns off the get/set
    // elimination pass (a32_interface).
    c.check_halt_on_memory_access = armrun_halt_on_access != 0;
    if (hooks->coproc) {                            // the FPA, on CP1 and CP2
        a->cp1 = std::make_shared<Fpa>(a.get(), 1);
        a->cp2 = std::make_shared<Fpa>(a.get(), 2);
        c.coprocessors[1] = a->cp1;
        c.coprocessors[2] = a->cp2;
    }
    c.code_cache_size = cache_bytes ? (cache_bytes < MIN_CACHE ? MIN_CACHE : cache_bytes) : DEFAULT_CACHE;
    a->jit = std::make_unique<A32::Jit>(c);
    return a.release();
}

void armrun_destroy(struct armrun *a)
{
    delete a;
}

uint32_t armrun_run(struct armrun *a, uint64_t ticks)
{
    a->ticks = ticks;
    std::uint32_t hr = (std::uint32_t)a->jit->Run();
    a->jit->ClearHalt(HaltReason::UserDefined1 | HaltReason::MemoryAbort);  // this run's: the next starts clean
    return hr & ARMRUN_STOP_MASK;
}

uint64_t armrun_ticks_left(const struct armrun *a)
{
    return a->ticks;
}

uint32_t armrun_step(struct armrun *a)
{
    a->ticks = 1;
    std::uint32_t hr = (std::uint32_t)a->jit->Step();
    a->jit->ClearHalt(HaltReason::UserDefined1 | HaltReason::MemoryAbort);
    return hr & ARMRUN_STOP_MASK;
}

void armrun_halt(struct armrun *a)
{
    a->jit->HaltExecution(HaltReason::UserDefined1);
}

void armrun_abort(struct armrun *a)
{
    a->jit->HaltExecution(HaltReason::MemoryAbort);
}

void armrun_invalidate(struct armrun *a, uint32_t addr, uint32_t len)
{
    a->jit->InvalidateCacheRange(addr, len);
}

void armrun_clear(struct armrun *a)
{
    a->jit->ClearCache();
}

uint32_t *armrun_regs(struct armrun *a)
{
    return a->jit->Regs().data();
}

uint32_t armrun_cpsr(const struct armrun *a)
{
    return a->jit->Cpsr();
}

void armrun_set_cpsr(struct armrun *a, uint32_t cpsr)
{
    a->jit->SetCpsr(cpsr);
}

uint32_t *armrun_ext(struct armrun *a)
{
    return a->jit->ExtRegs().data();
}

uint32_t armrun_fpscr(const struct armrun *a)
{
    return a->jit->Fpscr();
}

void armrun_set_fpscr(struct armrun *a, uint32_t fpscr)
{
    a->jit->SetFpscr(fpscr);
}

#ifdef DYNARMIC_EXTERNAL_SIGNALS
int dynarmic_handle_fault(int sig, siginfo_t *info, void *context);   // patches/0001
int armrun_handle_fault(int sig, void *siginfo, void *context)
{
    return dynarmic_handle_fault(sig, (siginfo_t *)siginfo, context);
}
#else
int armrun_handle_fault(int, void *, void *)
{
    return 0;
}
#endif

}  // extern "C"
