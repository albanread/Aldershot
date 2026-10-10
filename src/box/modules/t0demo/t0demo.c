/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* t0demo.c: t0demo.s, compiled to C by hand, exactly as the ObjAsm
 * compiler's tier 0 must compile it.
 *
 * This is the compiler's first golden test. Running `rosasm --emit c` on
 * t0demo.s must produce code equivalent to this file. ROSGD's runtime runs
 * this file now.
 *
 * The compiler's tier 0 follows these rules.
 *
 *   - Every instruction keeps its exact A32 semantics, working on the
 *     state block. The comment beside each line is its ObjAsm line.
 *   - Code is compiled in regions. A region is code joined by anything
 *     other than a call. SWIHandler's jump table branches into Sum,
 *     Classify, Fail and Greet, and Fail falls through into ReturnError.
 *     All of that is one C function made of labelled blocks, and it is
 *     entered only at SWIHandler.
 *   - Addresses are those of the ROM image. rom_t0demo.h holds the address
 *     of every label, from rosasm's own layout. ADR yields them, and the
 *     image's bytes are at them.
 *   - A SWI with a constant number is bound statically. A native kernel
 *     SWI is bound to its thunk. A module's SWI goes through ros_swi,
 *     which passes the register block straight to its compiled handler.
 *   - An indirect call goes through the dispatcher. Its return is checked
 *     against the address that the call expected.
 *   - Only the dispatcher's entries are visible outside this file. These
 *     are the addresses that something takes, such as module header
 *     offsets and DCD table words.
 *
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 Alban Read.
 */
#include "rosgd/api.h"
#include "rosgd/cpu.h"
#include "rom_t0demo.h"

/* Native kernel SWIs this module calls, bound statically. */
void ros_thunk_OS_Module(struct ros_cpu *s);
void ros_thunk_OS_Write0(struct ros_cpu *s);
void ros_thunk_OS_NewLine(struct ros_cpu *s);

#define R (s->r)

/* ---- region Init (t0demo.s:52) ---------------------------------------- */

static void Init(struct ros_cpu *s)
{
    R[13] -= 4; ros_st32(R[13], R[14]);         /* Init  STMFD  sp!, {lr}     */
    R[0] = 6;                                   /*       MOV    r0, #6        */
    R[3] = 16;                                  /*       MOV    r3, #16       */
    ros_thunk_OS_Module(s);                     /*       SWI    XOS_Module    */
    if (!s->v) ros_st32(R[12], R[2]);           /*       STRVC  r2, [r12]     */
    if (!s->v) R[0] = 0;                        /*       MOVVC  r0, #0        */
    if (!s->v) ros_st32(R[2], R[0]);            /*       STRVC  r0, [r2]      */
    R[15] = ros_ld32(R[13]); R[13] += 4;        /*       LDMFD  sp!, {pc}     */
}

/* ---- region Final (t0demo.s:61) ---------------------------------------- */

static void Final(struct ros_cpu *s)
{
    R[15] = R[14];                              /* Final MOV    pc, lr        */
}

/* ---- region SWIHandler (t0demo.s:66-139): entered only at SWIHandler ---- */

static void SWIHandler(struct ros_cpu *s)
{
    R[12] = ros_ld32(R[12]);                    /* LDR    r12, [r12]          */
    R[10] = ros_ld32(R[12]);                    /* LDR    r10, [r12]          */
    R[10] = R[10] + 1;                          /* ADD    r10, r10, #1        */
    ros_st32(R[12], R[10]);                     /* STR    r10, [r12]          */
    ros_subs(s, R[11], 4);                      /* CMP    r11, #4             */
    if (ros_cond(s, ROS_CC)) {                  /* ADDLO  pc, pc, r11, LSL #2 */
        switch (R[11]) {                        /*   the table below it       */
        case 0: goto Sum;                       /* B      Sum                 */
        case 1: goto Classify;                  /* B      Classify            */
        case 2: goto Fail;                      /* B      Fail                */
        case 3: goto Greet;                     /* B      Greet               */
        }
    }
    goto BadSWI;                                /* B      BadSWI              */

BadSWI:
    R[0] = T0DEMO_ErrorBadSWI;                  /* ADR    r0, ErrorBadSWI     */
    goto ReturnError;                           /* B      ReturnError         */

Sum:
    R[2] = R[0];                                /* MOV    r2, r0              */
    R[0] = 0;                                   /* MOV    r0, #0              */
    R[3] = R[1]; ros_nz(s, R[3]);               /* MOVS   r3, r1              */
    R[1] = 0;                                   /* MOV    r1, #0              */
    if (s->z) { R[15] = R[14]; return; }        /* MOVEQ  pc, lr              */
SumLoop:
    R[12] = ros_ld32(R[2]); R[2] += 4;          /* LDR    r12, [r2], #4       */
    R[0] = ros_adds(s, R[0], R[12]);            /* ADDS   r0, r0, r12         */
    R[1] = R[1] + 0 + s->c;                     /* ADC    r1, r1, #0          */
    R[3] = ros_subs(s, R[3], 1);                /* SUBS   r3, r3, #1          */
    if (!s->z) goto SumLoop;                    /* BNE    SumLoop             */
    R[15] = R[14]; return;                      /* MOV    pc, lr              */

Classify:
    ros_subs(s, R[0], 0);                       /* CMP    r0, #0              */
    if (s->z) R[0] = 0;                         /* MOVEQ  r0, #0              */
    if (s->z) { R[15] = R[14]; return; }        /* MOVEQ  pc, lr              */
    if (ros_cond(s, ROS_LT)) R[0] = 1;          /* MOVLT  r0, #1              */
    if (ros_cond(s, ROS_LT)) goto ClassifyDone; /* BLT    ClassifyDone        */
    ros_subs(s, R[0], 100);                     /* CMP    r0, #100            */
    if (!s->c) R[0] = 2;                        /* MOVLO  r0, #2              */
    if (s->c) R[0] = 3;                         /* MOVHS  r0, #3              */
ClassifyDone:
    R[12] = 1; ros_nz(s, R[12]);                /* MOVS   r12, #1  (C kept)   */
    R[15] = R[14]; return;                      /* MOV    pc, lr              */

Fail:
    R[0] = T0DEMO_ErrorDemo;                    /* ADR    r0, ErrorDemo       */
ReturnError:                                    /*   Fail falls through       */
    ros_msr_f(s, ROS_V_BIT);                    /* MSR    CPSR_f, #V_bit      */
    R[15] = R[14]; return;                      /* MOV    pc, lr              */

Greet:
    R[13] -= 8;                                 /* STMFD  sp!, {r4, lr}       */
    ros_st32(R[13], R[4]);
    ros_st32(R[13] + 4, R[14]);
    R[4] = R[12];                               /* MOV    r4, r12             */
    R[0] = T0DEMO_Message;                      /* ADR    r0, Message         */
    ros_thunk_OS_Write0(s);                     /* SWI    XOS_Write0          */
    ros_thunk_OS_NewLine(s);                    /* SWI    XOS_NewLine         */
    R[0] = 42;                                  /* MOV    r0, #42             */
    ros_swi(s, XT0Demo_Classify);               /* SWI    XT0Demo_Classify    */
    R[1] = T0DEMO_Handlers;                     /* ADR    r1, Handlers        */
    R[1] = ros_ld32(R[1] + (R[0] << 2));        /* LDR    r1, [r1, r0, LSL #2]*/
    R[0] = 21;                                  /* MOV    r0, #21             */
    R[14] = T0DEMO_Greet + 0x30;                /* MOV    lr, pc  (pc + 8)    */
    ros_call(s, R[1]);                          /* MOV    pc, r1              */
    if (R[15] != T0DEMO_Greet + 0x30)           /*   the return is checked    */
        ros_bad_return(s, T0DEMO_Greet + 0x30);
    R[1] = R[0];                                /* MOV    r1, r0              */
    R[0] = ros_ld32(R[4]);                      /* LDR    r0, [r4]            */
    R[4] = ros_ld32(R[13]);                     /* LDMFD  sp!, {r4, pc}       */
    R[15] = ros_ld32(R[13] + 4);
    R[13] += 8;
}

/* ---- regions Once and Twice (t0demo.s:147-150): entered by address ---- */

static void Once(struct ros_cpu *s)
{
    R[15] = R[14];                              /* Once  MOV    pc, lr        */
}

static void Twice(struct ros_cpu *s)
{
    R[0] = R[0] + R[0];                         /* Twice ADD    r0, r0, r0    */
    R[15] = R[14];                              /*       MOV    pc, lr        */
}

/* ---- the dispatcher's entries ------------------------------------------- */

static const struct ros_code_entry t0demo_code[] = {
    { T0DEMO_Init,       Init,       "T0Demo:Init" },
    { T0DEMO_Final,      Final,      "T0Demo:Final" },
    { T0DEMO_SWIHandler, SWIHandler, "T0Demo:SWIHandler" },
    { T0DEMO_Once,       Once,       "T0Demo:Once" },
    { T0DEMO_Twice,      Twice,      "T0Demo:Twice" },
};

void t0demo_register(void)
{
    ros_code_register(t0demo_code, sizeof t0demo_code / sizeof t0demo_code[0]);
}
