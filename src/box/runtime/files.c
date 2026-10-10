/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* files.c: the kernel's file SWIs, each through its vector.
 *
 * OS_File, OS_Args, OS_BGet, OS_BPut, OS_GBPB, OS_Find and OS_FSControl are
 * the kernel's only by number: it calls FileV, ArgsV, BGetV, BPutV, GBPBV,
 * FindV and FSCV, and FileSwitch owns them (modules/fileswitch).  The
 * kernel's default owners answer with an error, as here: until a filing
 * system manager claims the vectors, there are no files.
 */
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"

#define FILEV 0x08u
#define ARGSV 0x09u
#define BGETV 0x0Au
#define BPUTV 0x0Bu
#define GBPBV 0x0Cu
#define FINDV 0x0Du
#define FSCV  0x0Fu

#define ERR_NO_SELECTED_FS 0x40Bu

static void through(struct ros_cpu *s, uint32_t vector)
{
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    s->v = 0;
    if (!ros_vector_call(vector, s))
        ros_swi_fail(s, ros_error(ERR_NO_SELECTED_FS, "No selected filing system"));
    ros_svc_sp = outer_sp;
    s->r[10] = r10, s->r[11] = r11, s->r[12] = r12;
}

void ros_thunk_OS_File(struct ros_cpu *s)      { through(s, FILEV); }
void ros_thunk_OS_Args(struct ros_cpu *s)      { through(s, ARGSV); }
void ros_thunk_OS_BGet(struct ros_cpu *s)      { through(s, BGETV); }
void ros_thunk_OS_BPut(struct ros_cpu *s)      { through(s, BPUTV); }
void ros_thunk_OS_GBPB(struct ros_cpu *s)      { through(s, GBPBV); }
void ros_thunk_OS_Find(struct ros_cpu *s)      { through(s, FINDV); }
void ros_thunk_OS_FSControl(struct ros_cpu *s) { through(s, FSCV); }
