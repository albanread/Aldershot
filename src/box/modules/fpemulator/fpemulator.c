/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* fpemulator.c: FPEmulator, a native stub. It has the name, version and
 * SWIs of RISC OS 5.30's FPEmulator and emulates nothing (#114).
 *
 * The box needs no floating point emulation. Its compilers turn the FPA
 * instructions in translated and compiled code into the host's own floating
 * point. clib's k_body.c already answers as if FPEmulator were there.
 * Stock !Run files do need the module. They contain lines such as:
 *
 *     RMEnsure FPEmulator 4.03 RMLoad System:Modules.FPEmulator
 *     RMEnsure FPEmulator 4.03 Error This application requires FPEmulator 4.03 or later
 *
 * These lines fail if no module of that name exists. For that reason the
 * ports edited them out (PipeDream's !Run and SparkFS's). With this module
 * in the ROM the lines pass, as they do on 5.30, where FPEmulator is a ROM
 * module.
 *
 * The title is "FPEmulator". The help string is 5.30's byte for byte:
 * "FPEmulator", a tab, then "4.39 (30 Mar 2024) (1.13CELM)". The version
 * that RMEnsure compares is the first number, 4.39 (see *Help FPEmulator,
 * and *Modules on the farm). There are no commands, as 5.30's module has
 * none. The SWI chunk is &40480, as 5.30 names it:
 *
 *   FPEmulator_Version          R0 = 439, as 5.30's
 *   FPEmulator_ContextLength    R0 = 136, 5.30's (an FPA context's size)
 *   FPEmulator_DeactivateContext  R0 = the active context, or -1 for none
 *                               (5.30 gives a task with none -1). No
 *                               context is active afterwards.
 *   FPEmulator_ActivateContext  R0 is the context made active, kept as given
 *   FPEmulator_ChangeContext    R0 is the new context in, and the old one out
 *   FPEmulator_InitContext, _ExceptionDump, _Abort, _LoadContext,
 *   _SaveContext                do nothing and give no error. There are no
 *                               FPA registers to set, dump or save.
 *
 * The "active context" is a word kept here and nothing else. No FPA state
 * is behind it. SWIs 10-63 of the chunk give "SWI value out of range for
 * module FPEmulator", as a module's unknown SWIs do. */
#include <stdint.h>

#include "fpemulator.h"
#include "rosgd/api.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"

#define NO_CONTEXT 0xFFFFFFFFu

static uint32_t active = NO_CONTEXT;

void ros_thunk_FPEmulator_Version(struct ros_cpu *s)
{
    s->r[0] = FPEMULATOR_VERSION;
    s->v = 0;
}

void ros_thunk_FPEmulator_DeactivateContext(struct ros_cpu *s)
{
    s->r[0] = active;
    active = NO_CONTEXT;
    s->v = 0;
}

void ros_thunk_FPEmulator_ActivateContext(struct ros_cpu *s)
{
    active = s->r[0];
    s->v = 0;
}

void ros_thunk_FPEmulator_ChangeContext(struct ros_cpu *s)
{
    uint32_t old = active;
    active = s->r[0];
    s->r[0] = old;
    s->v = 0;
}

void ros_thunk_FPEmulator_ContextLength(struct ros_cpu *s)
{
    s->r[0] = FPEMULATOR_CONTEXT_LENGTH;
    s->v = 0;
}

#define NOTHING(n) \
    void ros_thunk_FPEmulator_##n(struct ros_cpu *s) { s->v = 0; }
NOTHING(InitContext)
NOTHING(ExceptionDump)
NOTHING(Abort)
NOTHING(LoadContext)
NOTHING(SaveContext)

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6, "SWI value out of range for module FPEmulator");
}

struct ros_module fpemulator_module = {
    .title = "FPEmulator",
    .help = "FPEmulator\t4.39 (30 Mar 2024) (1.13CELM)",
    .bad_swi = bad_swi,
    .swi_chunk = 0x40480,
    .swi_thunks = ros_swi_thunks_FPEmulator,
    .swi_names = ros_swi_names_FPEmulator,
    .swi_prefix = "FPEmulator",
};

__attribute__((constructor)) static void count(void)
{
    fpemulator_module.swi_count = ros_swi_count_FPEmulator;
}
