/* Copyright 2001 Pace Micro Technology plc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a translation into C of RISC OS Open's BBC BASIC source
 * (Sources/Programmer/BASIC: s.Basic, s.ModHead).
 */

/* basic_services.c: Basic_Services, the module's service call entry.
 * Translated by hand from the lift's body, label for label
 * (ModHead.s:257).  The lift inlined MOVEMEMORY from Basic.s into it.
 *
 * The one service it answers is Service_Memory (17), with which the
 * Wimp negotiates moving memory.  R2 names the active object.  The
 * handler runs the relocation only when that object is THIS module.
 * The relocation goes like this:
 *   - the stack is squeezed down to the FSA
 *   - everything is moved by the delta
 *   - the stack is stretched back up.
 * On the way, ERRSTK and the self-reference of every LOCALARLIST node
 * are patched by the delta.  Every refusal path is the original's.
 * Each is an LDMFD SP!,{R0,R2-R8,PC} that restores the pushed frame
 * and returns WITHOUT claiming the service.  The one success exit also
 * reloads R1 with Service_Memory.  So the original ends "not claimed",
 * and the Wimp continues its round.
 *
 * Register notes (the lift's own modelling, kept exactly):
 *   - The service's grow amount comes in R0, and is NEGATIVE for a
 *     shrink.  The pushed frame is {R0,R2-R8,R14} (36 bytes, R0
 *     lowest).
 *   - "LDR R14,[SP,#0]" in mid-body reloads the PUSHED R0 into the
 *     R14 local.  The lifter reused the register exactly as the
 *     original does.
 *   - STMIA SP,{SP}^ (the probe of the user-mode SP) is modelled as a
 *     store of (SP-4) at [SP-4], below the current pointer.  SP does
 *     not change.  The temporary word lives in the red zone, as in the
 *     lift.
 *   - The R15 protocol at entry and exit: plain returns with R15 = the
 *     entry R14.  There is no resume machinery anywhere in the body.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "basic_services.h"

/* The module's ROM window (the generated header's own constants). */
#define BS_MODULE_START 0xFC100000u
#define BS_MODULE_END   0xFC11021Cu

/* ARGP-relative variables (VARS = &8700). */
#define BS_MEMM        (-25)     /* the move-memory enable byte    */
#define BS_INSTALLLIST (-96)     /* INSTALL'd library list          */
#define BS_MEMLIMIT    (-84)
#define BS_HIMEM       (-132)
#define BS_ERRSTK      (-120)
#define BS_FSA         (-140)
#define BS_DIMLOCAL    (-36)
#define BS_LOCALARLIST_M4 (-104) /* LOCALARLIST-4 (the -4 pre-node) */

#define BS_VARS 0x8700u
#define BS_SERVICE_MEMORY 17u

/* The shared epilogue for refusals and the exit.  It does LDMFD
 * SP!,{R0,R2-R8,PC} off the pushed 36-byte frame.  Each site first
 * sets its own R1, R12 and R14. */
static void bs_pop_return(struct ros_cpu *s, uint32_t r13,
                          uint32_t r1, int set12, uint32_t r12,
                          int set14, uint32_t r14)
{
    s->r[1] = r1;
    if (set12)
        s->r[12] = r12;
    if (set14)
        s->r[14] = r14;
    s->r[0] = ros_ld32(r13);
    s->r[2] = ros_ld32(r13 + 4);
    s->r[3] = ros_ld32(r13 + 8);
    s->r[4] = ros_ld32(r13 + 12);
    s->r[5] = ros_ld32(r13 + 16);
    s->r[6] = ros_ld32(r13 + 20);
    s->r[7] = ros_ld32(r13 + 24);
    s->r[8] = ros_ld32(r13 + 28);
    s->r[13] = r13 + 36;
    s->r[15] = ros_ld32(r13 + 32);
}

void basicvfp_hand_Basic_Services(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r12 = s->r[12], r13 = s->r[13],
             r14 = s->r[14];

    ros_subs(s, r1, BS_SERVICE_MEMORY);        /* CMP R1,#Service_Memory */
    if (r1 != BS_SERVICE_MEMORY) {             /* MOVNE PC,R14 */
        s->r[0] = r0;
        s->r[15] = r14;
        return;
    }
    /* R2 is the active object.  Is it in this module
     * (ModuleStart..ModuleEnd)? */
    ros_subs(s, r2, BS_MODULE_START);          /* CMP R2,R12 */
    if (r2 >= BS_MODULE_START) {
        r12 = BS_MODULE_END;
        ros_subs(s, r12, r2);                  /* CMPHS R12,R2 */
    } else {
        r12 = BS_MODULE_START;
    }
    if (!(r2 >= BS_MODULE_START && r12 >= r2)) {
        s->r[0] = r0;                          /* MOV PC,R14 */
        s->r[12] = r12;
        s->r[15] = r14;
        return;
    }
    ros_logic(s, r0, s->c);                    /* TEQ R0,#0 */
    if ((int32_t)r0 >= 0) {                    /* MOVPL PC,R14 ; ignore grows */
        s->r[0] = r0;
        s->r[12] = r12;
        s->r[15] = r14;
        return;
    }

    r13 -= 36;                                 /* STMFD SP!,{R0,R2-R8,R14} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r2);
    ros_st32(r13 + 8, r3);
    ros_st32(r13 + 12, r4);
    ros_st32(r13 + 16, r5);
    ros_st32(r13 + 20, r6);
    ros_st32(r13 + 24, r7);
    ros_st32(r13 + 28, r8);
    ros_st32(r13 + 32, r14);

    r8 = BS_VARS;                              /* MOV ARGP,#VARS */
    r0 = ros_ld8(r8 + BS_MEMM);                /* LDRB R0,[ARGP,#MEMM] */
    r1 = ros_ld32(r8 + BS_INSTALLLIST);        /* LDR R1,[ARGP,#INSTALLLIST] */
    r4 = ros_ld32(r8 + BS_MEMLIMIT);           /* LDR R4,[ARGP,#MEMLIMIT] */
    r5 = ros_ld32(r8 + BS_HIMEM);              /* LDR R5,[ARGP,#HIMEM] */

    ros_logic(s, r0 & 1, s->c);                /* TST R0,#1 */
    if (s->z) {                                /* the move bit is off */
        r1 = 0;                                /* MOVEQ R1,#0 */
        bs_pop_return(s, r13, r1, 1, r12, 0, 0);
        return;
    }
    if (r1 != 0) {                             /* libraries installed */
        r1 = 0;                                /* MOVNE R1,#0 */
        bs_pop_return(s, r13, r1, 1, r12, 0, 0);
        return;
    }

    r0 = 14;                                   /* MOV R0,#14 */
    r1 = 0; r2 = 0; r3 = 0;
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;  /* SWI XOS_ChangeEnvironment */
    s->r[3] = r3; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_ChangeEnvironment);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    if (r1 != r4 || r1 != r5) {                /* TEQ R1,R4 ; TEQEQ R1,R5 */
        r1 = 0;                                /* not holding whole app space */
        bs_pop_return(s, r13, r1, 1, r12, 0, 0);
        return;
    }

    r4 = ros_ld32(r8 + BS_DIMLOCAL);           /* DIM LOCALs in use? */
    ros_st32(r13 - 4, r13 - 4);                /* STMIA SP,{SP}^ */
    ros_logic(s, r4, s->c);                    /* TEQ R4,#0 */
    if (r4 != 0)
        r1 = 0;                                /* MOVNE R1,#0 */
    r5 = ros_ld32(r13 - 4);                    /* LDR R5,[SP],#4 */
    if (r4 != 0) {
        bs_pop_return(s, r13, r1, 1, r12, 0, 0);
        return;
    }

    r4 = ros_ld32(r8 + BS_HIMEM);              /* LDR R4,[ARGP,#HIMEM] */
    r5 = r4 - r5;                              /* SUBS R5,R4,R5 */
    r6 = ros_ld32(r8 + BS_FSA);                /* LDR R6,[ARGP,#FSA] */
    r2 = r6 + 0x400u + r5;                     /* ADD R2,R6,#1024; ADD R2,R2,R5 */
    r14 = ros_ld32(r13);                       /* LDR R14,[SP,#0] ; the amount */
    r0 = ros_adds(s, r1, r14);                 /* ADDS R0,R1,R14 ; new end */
    if ((int32_t)r0 < 0) {                     /* wrapped below zero */
        r1 = 0;
        bs_pop_return(s, r13, r1, 1, r12, 1, r14);
        return;
    }
    r7 = r0 - 0x8000u;                         /* SUB R7,R0,#&8000 ; new slot */
    ros_subs(s, r2, r0);                       /* CMP R2,R0 */
    if (r2 >= r0) {                            /* going too small */
        r1 = 0;
        bs_pop_return(s, r13, r1, 1, r12, 1, r14);
        return;
    }

    s->irq_off = 0;                            /* CPSIE if, USR32_mode */
    ros_msr_c(s, (ros_cpsr(s) & ~0x1Fu) | 0x10u, 0xFC1002DCu);

    /* move the stack down to the FSA (ENDCHANGE1's trick) */
    do {
        uint32_t t;
        ROS_POLL(s, r13);
        t = ros_ld32(r13);                     /* LDR R3,[SP],#4 */
        r13 += 4;
        ros_st32(r6, t);                       /* STR R3,[R6],#4 */
        r6 += 4;
    } while (r13 < r4);                        /* BCC MOVEMEM1 */

    r7 = r0 - r13;                             /* SUB R7,R0,SP ; the delta */
    r13 = r0;                                  /* MOV SP,R0 */
    ros_st32(r8 + BS_MEMLIMIT, r0);            /* STR SP,[ARGP,#MEMLIMIT] */
    ros_st32(r8 + BS_HIMEM, r0);               /* STR SP,[ARGP,#HIMEM] */

    /* move the stack back up again */
    r1 = ros_ld32(r8 + BS_FSA);                /* LDR R1,[ARGP,#FSA] */
    do {
        uint32_t t;
        ROS_POLL(s, r13);
        r6 -= 4;                               /* LDR R3,[R6,#-4]! */
        r13 -= 4;                              /* STR R3,[SP,#-4]! */
        t = ros_ld32(r6);
        ros_st32(r13, t);
        ros_subs(s, r6, r1);                   /* CMP R6,R1 */
    } while (r6 > r1);                         /* BHI MOVEMEM2 */

    /* patch self references */
    r4 = r8 + BS_LOCALARLIST_M4;               /* ADD R4,ARGP,#LOCALARLIST-4 */
    r0 = ros_ld32(r8 + BS_ERRSTK) + r7;        /* ERRSTK += the delta */
    ros_st32(r8 + BS_ERRSTK, r0);
    for (;;) {
        ROS_POLL(s, r13);
        r3 = ros_ld32(r4 + 4);                 /* LDR R3,[R4,#4] ; next */
        ros_logic(s, r3, s->c);                /* TEQ R3,#0 */
        if (r3 == 0) {                         /* BEQ MOVEMEMLOCALARDONE */
            s->r[0] = r0; s->r[13] = r13;      /* SWI OS_EnterOS */
            ros_native_swi(s, ros_thunk_OS_EnterOS);
            if (s->v)
                ros_swi_raise(s);
            r0 = s->r[0];
            r1 = BS_SERVICE_MEMORY;            /* MOV R1,#Service_Memory */
            bs_pop_return(s, r13, r1, 0, 0, 1, r14);
            return;
        }
        r3 += r7;                              /* ADD R3,R3,R7 */
        ros_st32(r4 + 4, r3);                  /* STR R3,[R4,#4] */
        r4 = r3;                               /* MOV R4,R3 */
        r5 = ros_ld32(r3 + 8);                 /* LDR R5,[R4,#8] ; owner */
        r6 = ros_ld32(r5) + r7;                /* LDR R6,[R5]; ADD R6,R6,R7 */
        ros_subs(s, r6, r3 + 16);              /* CMP R6,R1 ; R1 = R4+16 */
        if (s->z)
            ros_st32(r5, r6);                  /* STREQ R6,[R5] */
    }
}
