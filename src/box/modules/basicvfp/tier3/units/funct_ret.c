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
 * (Sources/Programmer/BASIC: s.Funct).
 */

/* funct_ret.c -- BASIC's FN/PROC return unwinding, translated by hand
 * from Funct.s's GTARGS (RISC OS 5.31's BASIC, the VFP build). This is
 * the machinery under the value return of =FN and under ENDPROC.
 *
 * It is entered from FNRET with the head of the frame already popped,
 * and it walks the rest. The frame was built when the DEF was
 * executed. From the top down it is:
 *
 *   [list-end | tag][list-start]     the parameter markers
 *   [lv pairs...]                    RETURN destinations (tag &80..)
 *   [rv, type, lv of LOCALs...]      what LOCAL pushed
 *   [LINE][AELINE][FN/PROC]
 *
 * The tag's bits say what lies underneath:
 *   - a word below &80000000 is a LOCALS marker. RETSTK unwinds it.
 *     It may do so several times, because nested LOCALs stack several
 *     markers.
 *   - &C0000000 marks plain parameters.
 *   - &80000000 (bit 30 clear) marks parameters with RETURNs. This
 *     code recovers their destinations and values in the careful order
 *     below.
 *
 * The RETURN recovery (GTARGRET) has three passes:
 *   - The list is walked forward. The current value of every RETURN
 *     variable is pushed (VARIND, PUSHTYPE), and the count is kept.
 *   - The list is walked backward. Each parameter's l-value gets back
 *     the value the caller saw, from the destination area.
 *   - A PULLTYPE for each RETURN stores the pushed values INTO the
 *     caller's variables through the saved l-values. This is the
 *     actual "return" of a RETURN.
 * FNTEMPLOC holds the address of the lv list. FNTEMPLOC+4 holds the
 * frame end, which becomes SP.
 *
 * The quirks kept, with their lines:
 *   - the FN's own value is carried on the stack through all of this
 *     (PUSHTYPE at the top of GTARGRET, PULLTYPE at the very end);
 *   - a string rv is restored a word at a time into STRACC, with CLEN
 *     first (Funct.s's GTARGRETRESTRVSTR);
 *   - the last word pair is tested again for zero. A nonzero pair is an
 *     internal fault (ERRQ1), and is not raised as a BASIC error.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "funct_ret.h"

void basicvfp_RETSTK(struct ros_cpu *s);
void basicvfp_VARIND(struct ros_cpu *s);
void basicvfp_STOREA(struct ros_cpu *s);
void basicvfp_ERRQ1(struct ros_cpu *s);
void basicvfp_hand_PUSHTYPE(struct ros_cpu *s);
void basicvfp_hand_PULLTYPE(struct ros_cpu *s);

#define T_INTEGER 0x40000000u
#define T_FLOAT 0x80000000u
#define TFPLV 8u
#define STRACC_OFF 1536u

void basicvfp_hand_GTARGS(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3, r4, r5 = s->r[5], r6 = s->r[6],
             r7, r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11, r12,
             r13 = s->r[13], r14 = s->r[14];

    r11 = ros_ld32(r13);                    /* LDMFD SP!,{AELINE,LINE} */
    r12 = ros_ld32(r13 + 4);
    r4 = ros_ld32(r13 + 8);                 /* LDMFD SP!,{R4,R7} */
    r7 = ros_ld32(r13 + 12);
    r13 += 16;
    s->r[13] = r13;

    if (r4 == 0) {                          /* no args to replace */
        s->r[4] = r4; s->r[7] = r7; s->r[11] = r11; s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = r14;
        return;
    }
    r10 = r14;                              /* the link, saved */

    /* LOCALS markers: a word without bit 31 set */
    ros_subs(s, r4, 0x80000000u);
    while (r4 < 0x80000000u) {
        r6 = r4;                            /* this LOCALS group */
        s->r[4] = r4; s->r[6] = r6; s->r[7] = r7; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_RETSTK(s);                 /* unwind it */
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r5 = s->r[5]; r6 = s->r[6]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
        r13 = s->r[13]; r14 = s->r[14];
        r4 = ros_ld32(r13);                 /* the next marker */
        r7 = ros_ld32(r13 + 4);
        r13 += 8;
        s->r[13] = r13;
        if (r4 == 0) {                      /* nothing more at all */
            s->r[4] = r4; s->r[7] = r7; s->r[13] = r13;
            s->r[15] = r10;
            return;
        }
        ros_subs(s, r4, 0x80000000u);
    }

    ros_subs(s, r4, 0xC0000000u);
    r4 = (r4 & 0x3FFFFFFFu) - 8;            /* the list end, extracted */
    if (!s->c)
        goto GTARGRET;                      /* bit 30 clear: RETURNs */

    /* plain parameters: give each its l-value back */
    r5 = r7;                                /* the list start */
    do {
        r4 -= 8;                            /* LDMEA R4!,{R6,R7} */
        r6 = ros_ld32(r4);
        r7 = ros_ld32(r4 + 4) >> 16;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_RETSTK(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
        r14 = s->r[14];
        ros_subs(s, r4, r5);
    } while (r4 != r5);
    {
        uint32_t v1 = r13;
        r4 = ros_ld32(r13);                 /* LDMFD SP!,{R4,R7} */
        r7 = ros_ld32(r13 + 4);
        r13 += 8;
        s->r[13] = r13;
        ros_logic(s, ros_ld32(v1), s->c);   /* TEQ R4,#0 */
        if (r4 == 0) {
            s->r[4] = r4; s->r[7] = r7; s->r[13] = r13;
            s->r[15] = r10;
            return;
        }
        s->r[4] = r4; s->r[7] = r7; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERRQ1(s);
        return;
    }

GTARGRET: /* parameters with RETURNs */
    r6 = r4;                                /* the list end, again */
    r13 -= 8;                               /* keep hold of end and start */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r7);
    r5 = r13;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PUSHTYPE(s);              /* push the FN's value */
    r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13];
    r13 -= 4;
    ros_st32(r13, s->r[9]);                 /* its type */
    r13 -= 8;                               /* save {R10,AELINE} to free them */
    ros_st32(r13, r10);
    ros_st32(r13 + 4, r11);
    r11 = r5 + 8;                           /* the lv list's base */
    r10 = 0;                                /* the RETURN count */
    s->r[13] = r13;

    /* forward: push the RETURN variables' current values */
    do {
        r0 = ros_ld32(r7);                  /* LDMIA R7!,{IACC,TYPE} */
        r9 = ros_ld32(r7 + 4);
        r7 += 8;
        if (r9 & 0x400) {
            r9 >>= 16;
            r10 += 1;
            s->r[0] = r0; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
            s->r[9] = r9; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_VARIND(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
            r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_PUSHTYPE(s);
            r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4];
            r13 = s->r[13];
            r13 -= 4;
            ros_st32(r13, s->r[9]);
        }
    } while (r6 != r7);

    /* backward: restore each parameter's value from the lv area */
    r6 = ros_ld32(r5);                      /* the list again */
    r7 = ros_ld32(r5 + 4);
    r11 += r10 << 3;                        /* past the RETURN lvs */
    ros_st32(r8 - 404, r5 + 8);             /* FNTEMPLOC: the lv list */
    s->r[13] = r13;
    do {
        r6 -= 8;                            /* LDMEA R6!,{R4,R5} */
        r4 = ros_ld32(r6);
        r5 = ros_ld32(r6 + 4) >> 16;
        if (r5 == TFPLV) {                  /* a float */
            s->fp->vfp.d[0] = ros_ldd(r11);
            r11 += 8;
            r9 = T_FLOAT;
        } else if (r5 >= TFPLV) {           /* a string: CLEN then words */
            r2 = ros_ld32(r11);
            r11 += 4;
            r0 = r8 - STRACC_OFF;
            r1 = r2 - r0;
            if (r1 != 0) {
                r1 = (r1 + 3) & ~3u;
                do {
                    r3 = ros_ld32(r11);
                    r11 += 4;
                    ros_st32(r0, r3);
                    r0 += 4;
                    r1 -= 4;
                } while (r1 != 0);
            }
            r13 -= 8;
            ros_st32(r13, r6);
            ros_st32(r13 + 4, r7);
            r9 = 0;                         /* a string type */
            s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
            s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
            s->r[9] = r9; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_STOREA(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r8 = s->r[8]; r9 = s->r[9];
            r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
            r13 = s->r[13];
            r6 = ros_ld32(r13);             /* LDMFD SP!,{R6,R7} */
            r7 = ros_ld32(r13 + 4);
            r13 += 8;
            goto tail_check;
        } else {                            /* an integer */
            r0 = ros_ld32(r11);
            r11 += 4;
            r9 = T_INTEGER;
        }
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[7] = r7; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
        s->r[12] = r12; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_STOREA(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
tail_check:
        ros_subs(s, r6, r7);
    } while (r6 != r7);

    ros_st32(r8 - 400, r11);                /* FNTEMPLOC+4: the frame end */
    r11 = ros_ld32(r8 - 404);               /* the lv list again */

    /* store the pushed values into the RETURN variables */
    do {
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_PULLTYPE(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r9 = s->r[9]; r13 = s->r[13];
        r4 = ros_ld32(r11);                 /* LDMIA AELINE!,{R4,R5} */
        r5 = ros_ld32(r11 + 4);
        r11 += 8;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_STOREA(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
        r10 = ros_subs(s, r10, 1);
    } while (r10 != 0);

    r10 = ros_ld32(r13);                    /* LDMFD SP!,{R10,AELINE} */
    r11 = ros_ld32(r13 + 4);
    r13 += 8;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);              /* the FN's own value */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    {
        uint32_t v3 = ros_ld32(r8 - 400);   /* the frame end */
        r13 = v3;
        r4 = ros_ld32(r13);                 /* LDMFD SP!,{R4,R7} */
        r7 = ros_ld32(r13 + 4);
        r13 += 8;
        ros_logic(s, ros_ld32(v3), s->c);   /* TEQ R4,#0 */
        if (r4 == 0) {
            s->r[4] = r4; s->r[7] = r7; s->r[10] = r10; s->r[11] = r11;
            s->r[13] = r13;
            s->r[15] = r10;
            /* PERFORMANCE PHASE (P1): the RETURN-parameters path used
             * to call ros_resume(r10). That was a longjmp round trip
             * into the caller's BL frame, which had called setjmp. It
             * now returns through the C stack. This is safe for three
             * reasons:
             *  - R15 is already the link (the caller's BL site);
             *  - every caller's wrapper checks exactly that.
             *    fnret_gtargs and endpr_gtargs below have no check of
             *    their own, but carry on as after a normal call. The
             *    ros_check_return in the lift's wrappers passes;
             *  - the round trip went to the immediate caller anyway. */
            return;
        }
        s->r[4] = r4; s->r[7] = r7; s->r[10] = r10; s->r[11] = r11;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERRQ1(s);
    }
}
