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
 * (Sources/Programmer/BASIC: s.Basic, s.ErrorMsgs).
 */

/* basic_organs.c: the interpreter's small helper routines, translated
 * by hand from Basic.s (RISC OS 5.31's BASIC, the VFP build).  They
 * are:
 *   - the assignment store
 *   - the clears for variable initialisation and the cache
 *   - the stepping for FOR/NEXT and DATA
 *   - the two cache invalidators
 *   - the error-message builder
 *   - FSA set-up
 *   - line input
 *   - the banner
 *   - the reset of the error handler.
 * The bigger machines call these fourteen functions constantly.  With
 * them, the callees of CLRSTK and FNGOACACHE are mostly translated by
 * hand.  Those units make heavy use of setjmp, and need this before
 * they land.
 *
 * The quirks kept, with their lines:
 *   - MSG finds its error number and token in the two words after its
 *     call site (in Basic.s, the "= number, token" pairs of
 *     ErrorMsgs.s).  It reads them through R14.  So it works from any
 *     caller, lift or hand, that sets a real return address.
 *   - MSG builds "Enn" at STRACC and looks the token up (MSGXLATE).
 *     It shows the CLI that was running (OSCLIREGS).  It issues
 *     Service_Error with ErrorBase_BASIC ORed into the number.  Then
 *     it hands the block to MSGERR.
 *   - PURGECACHE has two algorithms.  For a name range under 256
 *     bytes, it probes exactly the cache slots that the bytes hash to.
 *     For a wider range, it sweeps all 256 slots.  Both run with
 *     interrupts off (MRS/MSR cpsr_f), as in the original.
 *   - DATA skips a statement a byte pair at a time, with two LDRBs per
 *     step.  So a line of odd length ends on CR exactly as in the
 *     original.
 *   - SETVAL clears VARPTR up to FNPTR+4 (62 words), and then all 256
 *     cache slots.  So a NEW clears every variable.
 *   - TITLE pushes all of R0-R12 and R14 around its OS_WriteS, because
 *     the banner may be written at almost any entry.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "basic_organs.h"

void basicvfp_STOREA(struct ros_cpu *s);
void basicvfp_MSGXLATE(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                       uint32_t *p2, uint32_t *p3, uint32_t r4, uint32_t r5,
                       uint32_t r6, uint32_t r7, uint32_t *p13, uint32_t r14);
void basicvfp_OSCLIREGS(struct ros_cpu *s, uint32_t *p1, uint32_t *p2,
                        uint32_t *p3, uint32_t *p4, uint32_t *p5, uint32_t r8,
                        uint32_t r12, uint32_t r13, uint32_t r14);
void basicvfp_MSGERR(struct ros_cpu *s);
void basicvfp_ESCAPE(struct ros_cpu *s);
void basicvfp_CTALLY(struct ros_cpu *s);
void basicvfp_FC1006C0(struct ros_cpu *s);
void basicvfp_ERSYNT(struct ros_cpu *s);
void basicvfp_hand_STMT(struct ros_cpu *s);
void basicvfp_hand_CRLINE(struct ros_cpu *s);
void basicvfp_hand_SETVAL(struct ros_cpu *s);
void basicvfp_hand_SETVAR(struct ros_cpu *s, uint32_t *p0, uint32_t r8,
                          uint32_t r14);
void ros_thunk_OS_ReadLine(struct ros_cpu *s);
void ros_thunk_OS_ServiceCall(struct ros_cpu *s);

#define TELSE 139u
#define CACHEMASK 255u
#define CACHECHECK 8u
#define VCACHE 0u
#define ErrorBase_BASIC 0x81FB00u
#define Service_Error 6u
#define ERRHAN 0xFC101444u                   /* ADR R0,ERRHAN */

/* STORE: pop the assignment's value and pointer, and pass them to
 * STOREA. */
void basicvfp_hand_STORE(struct ros_cpu *s)
{
    uint32_t r13 = s->r[13];
    s->r[4] = ros_ld32(r13);                 /* LDMFD SP!,{R4,R5} */
    s->r[5] = ros_ld32(r13 + 4);
    s->r[13] = r13 + 8;
    s->r[15] = s->r[14];
    basicvfp_STOREA(s);
}

/* SETVAL (Basic.s, the clear done by NEW): set the variable lists, the
 * library and overlay pointers, and the whole cache to zero. */
void basicvfp_hand_SETVAL(struct ros_cpu *s)
{
    uint32_t r8 = s->r[8];
    uint32_t r1 = r8 - 0x200u;
    uint32_t r2 = r1 + 244;                  /* up to FNPTR+4 */
    ros_st32(r8 - 92, 0);                    /* LIBRARYLIST */
    ros_st32(r8 - 88, 0);                    /* OVERPTR */
    do {
        ros_st32(r1, 0);
        r1 += 4;
    } while (r1 != r2);
    r1 = r8 + VCACHE;
    r2 = r1 + 0x1000u;
    do {
        ros_st32(r1, 0);
        r1 += 4;
    } while (r1 != r2);
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[15] = s->r[14];
}

/* SETVAR: DATAP := PAGE. */
void basicvfp_hand_SETVAR(struct ros_cpu *s, uint32_t *p0, uint32_t r8,
                          uint32_t r14)
{
    uint32_t r0 = ros_ld32(r8 - 148);        /* PAGE */
    ros_st32(r8 - 268, r0);                  /* DATAP */
    *p0 = r0;
    s->r[15] = r14;
}

/* DATA: skip to the next line, two bytes at a time. */
void basicvfp_hand_DATA(struct ros_cpu *s)
{
    uint32_t r10, r12 = s->r[12];
    do {
        r10 = ros_ld8(r12);
        r12 += 1;
        if (r10 == 13) {
            s->r[10] = r10;
            s->r[12] = r12;
            s->r[15] = s->r[14];
            basicvfp_hand_CRLINE(s);
            return;
        }
        r10 = ros_ld8(r12);
        r12 += 1;
    } while (r10 != 13);
    s->r[10] = r10;
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_CRLINE(s);
}

/* NXT: ':' continues and CR ends the line.  ELSE, or anything else,
 * skips a statement. */
void basicvfp_hand_NXT(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10];
    ros_subs(s, r10, 58);                    /* ":" */
    if (r10 == 58) {
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    if (r10 != 13) {
        s->r[15] = s->r[14];
        basicvfp_hand_DATA(s);
        return;
    }
    s->r[15] = s->r[14];
    basicvfp_hand_CRLINE(s);
}

/* DONEXT / DONXTS: decide where to go after a statement. */
void basicvfp_hand_DONEXT(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10];
    if (r10 == 32) {                         /* a space to consume */
        s->r[15] = s->r[14];
        basicvfp_hand_DONXTS(s);
        return;
    }
    ros_subs(s, r10, 58);
    if (r10 == 58) {
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    if (r10 == 13) {
        s->r[15] = s->r[14];
        basicvfp_hand_CRLINE(s);
        return;
    }
    ros_subs(s, r10, TELSE);
    if (r10 == TELSE) {
        s->r[15] = s->r[14];
        basicvfp_hand_DATA(s);
        return;
    }
    s->r[15] = s->r[14];
    basicvfp_ERSYNT(s);
}

void basicvfp_hand_DONXTS(struct ros_cpu *s)
{
    s->r[10] = ros_ld8(s->r[12]);
    s->r[12] += 1;
    s->r[15] = s->r[14];
    basicvfp_hand_DONEXT(s);
}

/* FLUSHCACHE: set the check word of every slot to zero. */
void basicvfp_hand_FLUSHCACHE(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                              uint32_t *p2, uint32_t r8, uint32_t r14)
{
    uint32_t r1 = r8 + 8;                    /* VCACHE+CACHECHECK */
    uint32_t r2 = r1 + 0x1000u;
    do {
        ros_st32(r1, 0);
        r1 += 16;
    } while (r1 != r2);
    *p0 = 0;
    *p1 = r1;
    *p2 = r2;
    s->r[15] = r14;
}

/* PURGECACHE: invalidate the slots a name range touches. */
void basicvfp_hand_PURGECACHE(struct ros_cpu *s, uint32_t r4, uint32_t *p5,
                              uint32_t *p6, uint32_t *p7, uint32_t r8,
                              uint32_t *p10, uint32_t r11, uint32_t r13,
                              uint32_t *p14)
{
    uint32_t r5 = 0, r6, r7, r10, cpsr;
    r13 -= 4;                                /* STR R14,[SP,#-4]! */
    ros_st32(r13, *p14);
    cpsr = ros_cpsr(s);                      /* MRS R14,CPSR */
    r10 = r11 - r4;
    if (r10 >= 0x100u) {
        /* sweep the whole cache, clearing entries inside the range */
        r6 = r8 + 8;
        r7 = r6 + 0x1000u;
        do {
            r10 = ros_ld32(r6);
            r6 += 16;
            if (r10 >= r4 && r11 >= r10)
                ros_st32(r6 - 16, r5);
        } while (r6 != r7);
    } else {
        /* probe only the slots that the range's own bytes hash to */
        r6 = r4;
        do {
            r7 = r8 + ((r6 & CACHEMASK) << 4);
            r10 = ros_ld32(r7 + CACHECHECK);
            if (r10 == r6)
                ros_st32(r7 + CACHECHECK, r5);
            r6 += 1;
        } while ((int32_t)r6 <= (int32_t)r11);
    }
    ros_msr_f(s, cpsr);                      /* MSR cpsr_f,R14 */
    *p5 = r5;
    *p6 = r6;
    *p7 = r7;
    *p10 = r10;
    *p14 = cpsr;
    /* The lift does NOT perform the SP writeback of the pop into PC.
     * Its callers absorb R13, so this leaves the stack as the lift
     * does */
    s->r[15] = ros_ld32(r13);                /* LDR PC,[SP],#4 */
}

/* MSG (Basic.s, the error builder).  The number and token are held in
 * the two words after the call site. */
void basicvfp_hand_MSG(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r9, r13 = s->r[13], r14 = s->r[14];
    uint32_t r4 = s->r[4], r5 = s->r[5], r8 = s->r[8];
    r3 = 0xFC10119Cu + r14 - 0xFC101198u;    /* the site's data word */
    r9 = r8 - 1536;                          /* STRACC */
    ros_st32(r9, ros_ld8(r3 - 4));           /* the error number */
    r0 = ros_ld8(r3 - 3);                    /* the token */
    r13 -= 8;                                /* the conversion buffer */
    r1 = r13 + 1;                            /* the gap for 'E' */
    r2 = 7;
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[13] = r13;
    ros_swi(s, 0x200D5u);                    /* XOS_ConvertCardinal1 */
    r0 = s->r[0];
    r2 = s->r[2];
    r0 -= 1;                                 /* the 'E' before the digits */
    ros_st8(r0, 69);
    r1 = r9 + 4;
    r3 = 252;
    r14 = 0xFC1011D0u;                       /* BL MSGXLATE */
    basicvfp_MSGXLATE(s, &r0, &r1, &r2, &r3, r4, r5, s->r[6], s->r[7],
                      &r13, r14);
    r13 += 8;
    /* Service_Error watchers see the internal error first */
    r13 -= 12;                               /* STMFD SP!,{R4-R5,R9} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    ros_st32(r13 + 8, r9);
    r14 = 0xFC1011DCu;                       /* BL OSCLIREGS */
    basicvfp_OSCLIREGS(s, &r1, &r2, &r3, &r4, &r5, r8, s->r[12], r13, r14);
    r14 = ros_ld8(r9);
    ros_st32(r9, ErrorBase_BASIC | r14);     /* the system-unique number */
    s->r[0] = r9;
    s->r[1] = Service_Error;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = r14;
    ros_native_swi(s, ros_thunk_OS_ServiceCall);
    r0 = s->r[0];
    r1 = s->r[1];
    r2 = s->r[2];
    r3 = s->r[3];
    r9 = s->r[9];
    r13 = s->r[13];
    r14 = s->r[14];
    ros_st32(r9, r14);                       /* our number back */
    r4 = ros_ld32(r13);                      /* LDMFD SP!,{R4-R5,R14} */
    r5 = ros_ld32(r13 + 4);
    r14 = ros_ld32(r13 + 8);
    r13 += 12;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = r14;
    basicvfp_MSGERR(s);
}

/* SETFSA: set LOMEM and FSA to the program's end, rounded up.  Clear
 * the free list and reset DATAP. */
void basicvfp_hand_SETFSA(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r6, r14, r8 = s->r[8];

    r0 = (ros_ld32(r8 - 144) + 3) & ~3u;     /* TOP */
    ros_st32(r8 - 136, r0);                  /* LOMEM */
    ros_st32(r8 - 140, r0);                  /* FSA */
    r6 = 0;
    r1 = r8 - 768;                           /* FREELIST */
    r2 = r1 + 0x100u;
    do {
        ros_st32(r1, r6);
        r1 += 4;
    } while (r1 < r2);
    r6 = s->r[14];                           /* the return, saved */
    r14 = 0xFC1010C0u;
    basicvfp_hand_SETVAR(s, &r0, r8, r14);
    s->r[14] = r6;
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[6] = r6;
    s->r[15] = r6;
    basicvfp_hand_SETVAL(s);
}

/* INLINE: read a line from the console into STRACC. */
void basicvfp_hand_INLINE(struct ros_cpu *s)
{
    uint32_t r8 = s->r[8];
    s->r[0] = r8 - 1536;
    s->r[1] = 238;
    s->r[2] = 32;
    s->r[3] = 0xFF;
    ros_native_swi(s, ros_thunk_OS_ReadLine);
    if (s->v)
        ros_swi_raise(s);
    if (s->c) {                              /* escape pressed */
        s->r[15] = s->r[14];
        basicvfp_ESCAPE(s);
        return;
    }
    s->r[1] = r8 - 1536;
    s->r[15] = s->r[14];
    basicvfp_CTALLY(s);
}

/* TITLE: print the banner, with all registers saved around it. */
void basicvfp_hand_TITLE(struct ros_cpu *s)
{
    uint32_t r13 = s->r[13], i;
    r13 -= 56;                               /* STMFD SP!,{R0-R12,R14} */
    for (i = 0; i < 13; i++)
        ros_st32(r13 + 4 * i, s->r[i]);
    ros_st32(r13 + 52, s->r[14]);
    s->r[13] = r13;
    ros_writes(s, 0xFC100698u);              /* SWI OS_WriteS */
    s->r[15] = s->r[14];
    basicvfp_FC1006C0(s);                    /* the restore stub */
}

/* ORDERR: set the error handler back to the default. */
void basicvfp_hand_ORDERR(struct ros_cpu *s, uint32_t *p0, uint32_t r8,
                          uint32_t r14)
{
    ros_st32(r8 - 124, ERRHAN);              /* ERRORH */
    *p0 = ERRHAN;
    s->r[15] = r14;
}
