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

/* funct_call.c -- BASIC's FN/PROC call entry, translated by hand from
 * Funct.s:281-430 (RISC OS 5.31's BASIC, the VFP build). It holds FN
 * and the FNBODY machinery. That is the call cache, the walk over the
 * name, and the search for the DEF through the main program, LIBRARY,
 * INSTALL and the overlays. Parameter passing and return (FNGOACACHE,
 * GTARGS) are the next unit. This one ends at FNGOA's cached tail.
 *
 * The original enters FNBODY in two ways. FN comes from the evaluator
 * and takes the lookup base from FNPTR. A PROC call enters directly,
 * with the base already in R1. Before anything else, a zero "end of
 * return info" word is pushed on the stack. This marks a FN call with
 * no link.
 *
 * The quirks kept, with their lines:
 *   - the call cache is keyed by the name's source address. It only
 *     stores an entry when ESCWORD is zero, so that TRACE is not
 *     fooled by cached jumps (Funct.s:324-326);
 *   - if a name's first character is below "@" or above "z", it is not
 *     a call at all. It gives "No such function/procedure" through MSG
 *     (FNCALL, Funct.s:302-305);
 *   - the DEF is searched for in the program, LIBRARY and INSTALL, and
 *     then every overlay in turn. An overlay is loaded from its file
 *     only when its name matches (Funct.s:328-378);
 *   - the found pointer is moved past the name and word-aligned
 *     (ADD/BIC at Funct.s:320) to reach the info triple (list, 0,
 *     addr).
 */

#include <stdio.h>
#include <string.h>
extern char fnt_trace[3000];
extern int fnt_pos;
static void FB(const char *tag, uint32_t a, uint32_t b)
{
    if (fnt_pos < 2900)
        fnt_pos += snprintf(fnt_trace + fnt_pos, 3000 - fnt_pos,
                            "%s %08x %08x\n", tag, a, b);
}
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "funct_call.h"

void basicvfp_FNGOACACHE(struct ros_cpu *s);
void basicvfp_FNGOA(struct ros_cpu *s);
void basicvfp_FNTRC(struct ros_cpu *s);
void basicvfp_FNFIND(struct ros_cpu *s, uint32_t *p0, uint32_t *p2,
                     uint32_t r3, uint32_t r4, uint32_t *p5, uint32_t *p6,
                     uint32_t *p7, uint32_t r10, uint32_t r11, uint32_t *p14);
void basicvfp_FNDEFLIST(struct ros_cpu *s);
void basicvfp_FNINSTANT(struct ros_cpu *s);
void basicvfp_FNMISS(struct ros_cpu *s);
void basicvfp_VARSTR(struct ros_cpu *s);
void basicvfp_OSFILELOADSTRACC(struct ros_cpu *s);
void basicvfp_hand_LOOKP1(struct ros_cpu *s);
void basicvfp_hand_FLUSHCACHE(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                              uint32_t *p2, uint32_t r8, uint32_t r14);
void basicvfp_hand_MSG(struct ros_cpu *s);

/* The FNRET fix in f93918e asks the runtime to hand over the registers
 * that a resume jump carried (ros_resume_take, in the runtime). The older trees, such as the Mac Pro's box-build, hold no
 * registers. There the weak reference stays null and the take does
 * nothing. That is exactly the return through C which those trees were
 * verified with. The declaration is repeated here instead of being put
 * under #ifdef. Where cpu.h has its own declaration, it agrees with
 * this weak one. */
extern void ros_resume_take(struct ros_cpu *s) __attribute__((weak));

#define CACHEMASK 255u

/* PERFORMANCE PHASE (P1): the guarded FN-call frame. The evaluator no
 * longer has a setjmp frame for each BL (expr_factor_rest.c's
 * EFR_RESUME). The FN's value normally returns through the C stack
 * (FNRET/GTARGS). But ONE case still needs a resume point at the call
 * site. That is an ON ERROR [LOCAL] handler whose statement is itself
 * a return (`ON ERROR LOCAL =-1`, errors.bas's FNsafe).
 *
 * An error raised inside the body runs the handler statement IN PLACE.
 * This is deeper on the C stack than the error site, because the
 * lift's MSGERR re-enters at LINE=ERRORH through its B STMT. So the C
 * return from FNRET unwinds only as far as the error machinery's
 * frame. The checked-return safety net there (ZDIVOR and the like)
 * sees R15 set to the FN-call site, and searches the chain for it.
 * THIS frame is what it finds. The longjmp lands here and the frame
 * pops. The value in the registers then goes up to the caller exactly
 * as the C return would have carried it.
 *
 * The handler can be set up inside the very call that needs it (FNsafe
 * sets up its own). So the frame cannot depend on the caller's state.
 * Every FN call pushes one, and it is the only setjmp left on the call
 * path. */
__attribute__((noinline)) static void fn_call_frame(struct ros_cpu *s, uint32_t sp_at_call)
{
    struct ros_resume rs;
    rs.at = s->r[14];
    /* This is the SP at the call, before this FN's own push. The lift's
     * BL sites record that value, and FNRET's MOV PC,R7 happens with
     * the frame popped. So the pair (site, sp) names THIS level. An FN
     * that recurses leaves one resume point per level at the same
     * address. */
    rs.sp = sp_at_call;
    rs.prev = ros_resume_top;
    if (setjmp(rs.jb) == 0) {
        ros_resume_top = &rs;
        basicvfp_hand_FNBODY(s);
        ros_resume_top = rs.prev;
    } else {
        /* This point was reached by a jump and not by a return. The
         * registers the jump carried belong to the jumper's register
         * file, which is not this one. An ON ERROR LOCAL handler that
         * returns from the FN runs on the register file that the error
         * handler was entered with, and the value comes across here.
         * The lift's BL sites take the registers in their
         * ros_check_return. P1 left this frame without one, so it must
         * take them itself, where the runtime holds any. */
        ros_resume_top = rs.prev;
        if (ros_resume_take)
            ros_resume_take(s);
    }
}

/* FN: the function case. The base comes from FNPTR and the link is
 * pushed. */
void basicvfp_hand_FN(struct ros_cpu *s)
{
    uint32_t sp_at_call = s->r[13];
    s->r[1] = ros_ld32(s->r[8] - 272);     /* FNPTR */
    s->r[13] -= 4;
    ros_st32(s->r[13], s->r[14]);
    s->r[15] = s->r[14];
    fn_call_frame(s, sp_at_call);          /* P1: the guarded call frame */
}

/* Is ch a character of a FN/PROC name? This follows the original's
 * cascade of compares: <= "z" and (>= "_" or (<= "Z" and (>= "@" or
 * digit))). */
static int namech(uint32_t ch)
{
    if (ch > 122)
        return 0;
    if (ch >= 95)
        return 1;                           /* _ ` a-z */
    if (ch > 90)
        return 0;
    if (ch >= 64)
        return 1;                           /* @ A-Z */
    if (ch > 57)
        return 0;
    return ch >= 48;                        /* 0-9 */
}

void basicvfp_hand_FNBODY(struct ros_cpu *s)
{
    uint32_t r0, r1 = s->r[1], r2, r3, r4, r5, r6, r7, r9,
             r11 = s->r[11], r13 = s->r[13], r14 = s->r[14],
             r8 = s->r[8];

    r13 -= 4;                               /* the zero return-info word */
    ros_st32(r13, 0);
    s->r[13] = r13;

    /* the call cache */
    {
        uint32_t slot = r8 + ((r11 & CACHEMASK) << 4);
        r0 = ros_ld32(slot);
        r4 = ros_ld32(slot + 8);
        r5 = ros_ld32(slot + 12);
        r2 = ros_ld32(slot + 4);
        if (r4 == r11) {
            s->r[11] = r11 + r2;            /* ADDEQ AELINE,AELINE,R2 */
            s->r[0] = r0;
            s->r[2] = r2;
            s->r[4] = r4;
            s->r[5] = r5;
            s->r[15] = s->r[14];
                    basicvfp_FNGOACACHE(s);
            return;
        }
    }

    /* the miss: find the DEF */
    r13 -= 4;
    ros_st32(r13, r11);
    r3 = ros_ld8(r11);
    r11 += 1;
    r4 = r11;
    s->r[13] = r13;
    if (!namech(r3))
        goto FNCALL;
    for (;;) {
        r5 = ros_ld8(r11);
        r11 += 1;
        if (namech(r5))
            continue;
        r11 -= 1;                           /* the terminator back */
        break;
    }
    r5 = ros_ld32(r8 - 112);                /* ESCWORD */
    ros_subs(s, r5, 0);
    if (r5 != 0) {                          /* BLNE FNTRC */
        s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[11] = r11; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_FNTRC(s);
        r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    }
    s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LOOKP1(s);                /* BL LOOKP1; R1 preserved */
    r0 = s->r[0]; r2 = s->r[2]; r5 = s->r[5]; r6 = s->r[6];
    r14 = s->r[14];
    if (!s->z) {
        /* not in the PROC list: search for the DEF everywhere */
        r13 = s->r[13];
        r13 -= 4;                           /* "not in overlay" */
        ros_st32(r13, 0xFFFFFFFF);
        r5 = ros_ld32(r8 - 148);            /* PAGE: the program */
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_FNFIND(s, &r0, &r2, r3, r4, &r5, &r6, &r7, s->r[10],
                        r11, &r14);
        if (!s->z) {
            s->r[0] = r0; s->r[2] = r2; s->r[5] = r5; s->r[6] = r6;
            s->r[7] = r7; s->r[13] = r13; s->r[14] = r14;
            s->r[15] = s->r[14];
            basicvfp_FNINSTANT(s);
            return;
        }
        r1 = ros_ld32(r8 - 92);             /* LIBRARYLIST */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[5] = r5;
        s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_FNDEFLIST(s);
        r1 = ros_ld32(r8 - 96);             /* INSTALLLIST */
        s->r[1] = r1;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_FNDEFLIST(s);
        r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r11 = s->r[11];
        r13 = s->r[13]; r14 = s->r[14];
        r9 = ros_ld32(r8 - 88);             /* OVERPTR */
        ros_logic(s, r9, s->c);             /* TEQ TYPE,#0 */
        if (r9 == 0) {
            s->r[9] = r9;
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_FNMISS(s);
            return;
        }
        r5 = ros_ld32(r9 + 4);              /* the current overlay */
        ros_adds(s, r5, 1);                 /* CMN R5,#1 */
        if (r5 != 0xFFFFFFFF) {
            ros_st32(r13, r5);
            r5 = r9 + 12;                   /* the overlay block */
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_FNFIND(s, &r0, &r2, r3, r4, &r5, &r6, &r7, s->r[10],
                            r11, &r14);
            if (!s->z) {
                s->r[0] = r0; s->r[2] = r2; s->r[5] = r5; s->r[6] = r6;
                s->r[7] = r7; s->r[9] = r9; s->r[14] = r14;
                s->r[15] = s->r[14];
                basicvfp_FNINSTANT(s);
                return;
            }
        }
        r1 = ros_ld32(r9);                  /* the overlay name array */
        r6 = 0;
        do {
            ros_st32(r13, r6);              /* the overlay index */
            r5 = r9 + 12;
            r0 = r1 + r6 + (r6 << 2);       /* the name, 5 words in */
            r13 -= 20;                      /* STMFD {R1,R3,R4,R5,TYPE} */
            ros_st32(r13, r1);
            ros_st32(r13 + 4, r3);
            ros_st32(r13 + 8, r4);
            ros_st32(r13 + 12, r5);
            ros_st32(r13 + 16, r9);
            s->r[13] = r13;
            s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
            s->r[9] = r9;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_VARSTR(s);             /* the name, as a string */
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r9 = s->r[9]; r14 = s->r[14];
            r4 = r2 - r8;
            r4 = ros_adds(s, r4, 1536);     /* SUBS R4,R4,#STRACC */
            if (r4 == 0) {                  /* the empty name: skip */
                r1 = ros_ld32(r13);
                r3 = ros_ld32(r13 + 4);
                r4 = ros_ld32(r13 + 8);
                r5 = ros_ld32(r13 + 12);
                r9 = ros_ld32(r13 + 16);
                r13 += 20;
            } else {
                ros_st8(r2, 13);            /* CR at CLEN */
                r2 = r5;
                s->r[0] = 13; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
                s->r[5] = r5; s->r[6] = r6; s->r[13] = r13;
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_OSFILELOADSTRACC(s);   /* load the overlay */
                r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
                r5 = s->r[5]; r6 = s->r[6]; r14 = s->r[14];
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_hand_FLUSHCACHE(s, &r0, &r1, &r2, r8, s->r[14]);
                r1 = ros_ld32(r13);         /* LDMFD SP,{...} */
                r3 = ros_ld32(r13 + 4);
                r4 = ros_ld32(r13 + 8);
                r5 = ros_ld32(r13 + 12);
                r9 = ros_ld32(r13 + 16);
                ros_st32(r9 + 4, r6);       /* the current overlay */
                s->r[13] = r13;
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_FNFIND(s, &r0, &r2, r3, r4, &r5, &r6, &r7,
                                s->r[10], r11, &r14);
                r1 = ros_ld32(r13);         /* LDMFD SP!,{...} */
                r3 = ros_ld32(r13 + 4);
                r4 = ros_ld32(r13 + 8);
                r5 = ros_ld32(r13 + 12);
                r9 = ros_ld32(r13 + 16);
                r13 += 20;
                if (!s->z) {
                    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
                    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
                    s->r[9] = r9; s->r[13] = r13; s->r[14] = r14;
                    s->r[15] = s->r[14];
                    basicvfp_FNINSTANT(s);
                    return;
                }
            }
            r6 = ros_ld32(r13) + 1;         /* the next overlay */
            r7 = ros_ld32(r1 - 4);          /* the count */
            ros_subs(s, r6, r7);
        } while (r6 < r7);
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[9] = r9; s->r[13] = r13; s->r[14] = r14;
        s->r[15] = s->r[14];
        basicvfp_FNMISS(s);                 /* nowhere at all */
        return;
    }
    /* found in the list: the info triple, word-aligned */
    r0 = (r0 + 3) & ~3u;
    s->r[0] = r0;
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_FNGOA(s);
    return;

FNCALL: /* not a name at all: "No FN/PROC" through MSG */
    s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_MSG(s);
    /* the lift faults here, because MSG does not return */
    ros_fault(s, 0xFC1100B4u, "a transfer to an address that is not code");
}
