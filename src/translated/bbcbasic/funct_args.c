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

/* funct_args.c -- BASIC's FN/PROC parameter passing and call entry,
 * translated by hand from Funct.s:330-520 (RISC OS 5.31's BASIC, the
 * VFP build). This is the FNGOACACHE machinery.
 *
 * It is entered from FNBODY with IACC holding the address of the info
 * triple (list, 0, DEF address) and R5 the character after the name.
 * If that character is not "(", DOFN runs the call directly. Otherwise
 * the actual parameters are evaluated from left to right, and each is
 * pushed as a typed value. They are then assigned from right to left
 * to the formal parameters' saved l-values. Each item takes one
 * STOREA, and the list is walked backwards with LDMEA.
 *
 * There are two evaluation loops, as in the source. The simple one
 * handles values only. The hard one is entered at the first RETURN or
 * array parameter. It cannot go back to the simple copy, exactly as
 * the original's duplicated code cannot. RETURN destinations are
 * stacked as l-values. At assignment time they are moved through a
 * temporary stack 256 bytes below SP (FNTEMPLOC). They are then pushed
 * as the frame's return information. The &40000000 bit stops the frame
 * being read as "has locals".
 *
 * The quirks kept, with their lines:
 *   - the check for stack room compares FSA+1024 against SP, and gives
 *     "Not enough stack to call function/procedure" (Funct.s:330-334);
 *   - the TYPE pushed for a formal parameter is the top half of the
 *     block word (LSR #16);
 *   - an array parameter matches on the block's type word with the
 *     count bits masked off (&FFF00FFF). Otherwise it gives "Bad array"
 *     (FNARGA);
 *   - the pair pushed for an array has TYPE below and the pointer
 *     above. There are two STRs at [SP,#-4]!, the pointer first, so the
 *     4 lands at the lower word. That is the order PULLTYPE pulls them
 *     in (FNARGA's array exit);
 *   - a RETURN of something that is not an l-value at all gives
 *     "Argument mismatch". So does a silly one (EQ,CS). But a name that
 *     can be created is created (Funct.s:461-470);
 *   - if the number of arguments does not reach the end of the list,
 *     the result is "Argument mismatch" (ARGMAT), from whichever loop
 *     found it;
 *   - the overlay path at DOFN loads the overlay file again when the
 *     DEF is not in the current overlay (DOFNOVERLAY).
 */

#include <stdio.h>
#include <stdio.h>
#include <string.h>
char fnt_trace[3000];                 /* the push/pop trace */
int fnt_pos;
/* PERFORMANCE PHASE (P2): the 9c push/pop trace tap is OFF. It used to
 * run on every call. It did one snprintf for each FN/PROC entry and one
 * for each traced point, at 18 sites. Once P1's setjmp frames were
 * gone, it was the biggest single cost left on the call path. The
 * profiler put it at about 20% of the procs bench. To turn it on again,
 * define FNT_TRACE_ON and restore the function below. The raise_at dump
 * works either way, because fnt_pos stays 0 when the tap is off. */
#if FNT_TRACE_ON
static void TR(const char *tag, uint32_t a, uint32_t b)
{
    if (fnt_pos < (int)sizeof fnt_trace - 60)
        fnt_pos += snprintf(fnt_trace + fnt_pos, sizeof fnt_trace - fnt_pos,
                            "%s %08x %08x\n", tag, a, b);
}
#else
#define TR(tag, a, b) ((void)0)
#endif
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"
#include "rosgd/task.h"             /* ros_stack_room: the FNGOACACHE hook */

#include "funct_args.h"

void basicvfp_EXPR(struct ros_cpu *s);
void basicvfp_VARIND(struct ros_cpu *s);
void basicvfp_STOREA(struct ros_cpu *s);
void basicvfp_ERRQ1(struct ros_cpu *s);
void basicvfp_FNMISS(struct ros_cpu *s);
void basicvfp_VARSTR(struct ros_cpu *s);
void basicvfp_OSFILELOADSTRACC(struct ros_cpu *s);
void basicvfp_hand_MSG(struct ros_cpu *s);
void basicvfp_hand_STMT(struct ros_cpu *s);
void basicvfp_hand_PUSHTYPE(struct ros_cpu *s);
void basicvfp_hand_PULLTYPE(struct ros_cpu *s);
void basicvfp_hand_CREATE(struct ros_cpu *s);
void basicvfp_hand_LVBLNK(struct ros_cpu *s);
void basicvfp_hand_FLUSHCACHE(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                              uint32_t *p2, uint32_t r8, uint32_t r14);

/* The real addresses of the MSG error sites. MSG reads its error
 * number and token from the words after these. */
#define SITE_DEEPPROC 0xFC1100FCu
#define SITE_ARGMAT   0xFC1100BCu
#define SITE_ARGMATARR 0xFC1100CCu
#define SITE_ARGMATRET 0xFC1100C4u
#define SITE_ERSIZE   0xFC10FF5Cu

/* Raise the site's own error, as the lift's BL MSG there does. MSG
 * reads the number and token from the words after the site, so R14 is
 * set to the site.
 *
 * (The tap's trace used to be dumped here as the text of a fault, and
 * that RAISED the fault. ros_fault does not return. So every error
 * through this helper came out as the runtime's "Compiled code at
 * &FC1100FC:" instead of BASIC's own. That covered ERDEEPPROC's "No
 * room for function/procedure call", the three ARGMAT spellings and
 * ERSIZE. With the P2 tap off the text was empty as well. The dump
 * belongs to the tap, so it is now inside it. rosgd's self-test has
 * the FN-recursion case, #64.) */
static void raise_at(struct ros_cpu *s, uint32_t site)
{
    TR("RAISE", site, 0);
#if FNT_TRACE_ON
    {
        char buf[3200];
        memcpy(buf, fnt_trace, fnt_pos); buf[fnt_pos] = 0;
        s->r[14] = site;
        ros_fault(s, site, buf);
    }
#endif
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

void basicvfp_hand_FNGOACACHE(struct ros_cpu *s)
{
    TR("ENTER", s->r[0], s->r[10]);
    uint32_t r0 = s->r[0], r1, r2 = s->r[2], r3 = s->r[3], r4, r5 = s->r[5],
             r6 = s->r[6], r7 = s->r[7], r8 = s->r[8], r9 = s->r[9],
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14 = s->r[14];

    r4 = ros_ld32(r8 - 140) + 0x400u;       /* FSA + 1024 room to work */
    ros_subs(s, r4, r13);
    /* Also check the thread's native stack, as the toolchain's lift does
     * here (the FNGOACACHE hook in modules/basicvfp/patch-basicasm.py,
     * #64). A recursion too deep for it then gives BASIC's own "No room
     * for function/procedure call" instead of a SIGSEGV. The hand body
     * replaces the patched one, so it must carry the patch.
     *
     * A tree whose task.h has no ROS_STACK_BASIC checks only BASIC's
     * stack, as its own lift does. The test is on the MACRO beside the
     * enum, and never on ROS_STACK_BASIC itself. That is an enum
     * constant, which #ifndef cannot see, so a fallback #define of it
     * would always take effect. ros_stack_room would then be asked for
     * the wrong budget. It would get the budget for an alias expansion,
     * which includes the SVC stack, where BASIC's calls are meant to
     * ask for none. */
#ifdef ROS_STACK_BASIC_NATIVE
    if (r4 >= r13 || !ros_stack_room(ROS_STACK_BASIC)) {
#else
    if (r4 >= r13) {
#endif
        s->r[4] = r4;
        raise_at(s, SITE_DEEPPROC);         /* too deep to call */
    }

    ros_subs(s, r5, 40);                    /* "(" : parameters? */
    if (r5 != 40) {
        r11 -= 1;
        goto DOFN;
    }

    /* FNARGS: the info block, then the formals' l-values pushed */
    r6 = r0;
    r0 = ros_ld32(r6);
    r9 = ros_ld32(r6 + 4);
    r5 = r6 + 8;
    if (r0 != 0) {
        do {
            r9 >>= 16;                      /* the store type */
            s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
            s->r[9] = r9;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_VARIND(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
            r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_PUSHTYPE(s);
            r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13];
            r0 = ros_ld32(r5);
            r9 = ros_ld32(r5 + 4);
            r5 += 8;
        } while (r0 != 0);
    }
    ros_st32(r13 - 4, r6);                  /* the list's beginning */
    r13 -= 16;                              /* {R5, R10, R5|&C0000000} */
    ros_st32(r13, r5);
    ros_st32(r13 + 4, r10);
    ros_st32(r13 + 8, r5 | 0xC0000000u);
    s->r[13] = r13;
    r7 = r6;                                /* the list start, saved */
    TR("triple", r13, r5);

    /* the simple loop: values only, until a RETURN/array appears */
    for (;;) {
        r4 = ros_ld32(r6);
        r5 = ros_ld32(r6 + 4);
        r6 += 8;
        if (r4 == 0)
            raise_at(s, SITE_ARGMAT);       /* more formals than args */
        r13 -= 8;
        ros_st32(r13, r6);
        ros_st32(r13 + 4, r7);
        s->r[13] = r13;
        if (r5 & 0x100)
            goto FNARGA;                    /* hard from here on */
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[7] = r7; s->r[9] = r9; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPR(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
        r13 = s->r[13]; r14 = s->r[14];
        r6 = ros_ld32(r13);
        r7 = ros_ld32(r13 + 4);
        r13 += 8;
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_PUSHTYPE(s);          /* the value */
        r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4];
        r13 = s->r[13]; r14 = s->r[14];
        r13 -= 4;
        ros_st32(r13, s->r[9]);             /* its type */
        s->r[13] = r13;
        ros_subs(s, s->r[10], 44);          /* "," */
        if (s->r[10] == 44)
            continue;                       /* the next argument */
        ros_subs(s, s->r[10], 41);          /* ")" */
        r5 = ros_ld32(r6);
        if (s->r[10] == 41)
            ros_logic(s, r5, s->c);         /* and the list's end */
        if (s->r[10] != 41 || r5 != 0)
            raise_at(s, SITE_ARGMAT);
        /* FNARGZ: assign right to left */
        do {
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_PULLTYPE(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r9 = s->r[9]; r13 = s->r[13]; r14 = s->r[14];
            r6 -= 8;                        /* backwards down the list */
            r4 = ros_ld32(r6);
            r5 = ros_ld32(r6 + 4);
            r13 -= 8;
            ros_st32(r13, r6);
            ros_st32(r13 + 4, r7);
            r5 >>= 16;                      /* the store type */
            s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_STOREA(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r8 = s->r[8]; r9 = s->r[9];
            r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            r6 = ros_ld32(r13);
            r7 = ros_ld32(r13 + 4);
            r13 += 8;
            ros_subs(s, r6, r7);
        } while (r6 != r7);
        r0 = ros_ld32(r13);                 /* the pushed info start */
        r10 = ros_ld32(r13 + 4);
        r13 -= 4;                           /* push {R10,AELINE,LINE} */
        ros_st32(r13, r10);
        ros_st32(r13 + 4, r11);
        ros_st32(r13 + 8, r12);
        r12 = ros_ld32(r0 - 4);             /* the DEF's line */
        r6 = ros_ld32(r0);
        ros_adds(s, r6, 1);
        s->r[0] = r0; s->r[6] = r6; s->r[7] = r7; s->r[10] = r10;
        s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        if (r6 == 0xFFFFFFFF) {
            basicvfp_hand_STMT(s);
            return;
        }
        goto DOFNOVERLAY;
    }

FNARGA: /* a RETURN or array parameter: the hard loops from here */
    for (;;) {
        TR("arg-array", r5, r13);
        if (r5 & 0x8000) {                  /* an array */
            r13 -= 4;
            ros_st32(r13, r5);
            s->r[13] = r13;
            s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
            s->r[7] = r7; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_LVBLNK(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
            r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
            TR("arr-lvblk", s->z, s->c);
            if (s->z)                       /* not an l-value at all */
                raise_at(s, SITE_ARGMATARR);
            ros_subs(s, r9, 0x100);
            TR("arr-type", r9, s->r[0]);
            if (r9 >= 0x100) {
                uint32_t want = ros_ld32(r13) & 0xFFF00FFFu;
                r13 += 4;
                ros_subs(s, r9, want);
                TR("arr-want", want, r9);
                if (r9 != want)
                    raise_at(s, SITE_ERSIZE);
                r6 = ros_ld32(r13);         /* the block and info ptr */
                r7 = ros_ld32(r13 + 4);
                r13 += 8;                   /* LDMFD's writeback */
                r0 = ros_ld32(r0);          /* the array's base */
                TR("arr-base", r0, r6);
                r13 -= 8;
                ros_st32(r13, 4);           /* the type goes BELOW the
                 * value. STR TYPE is the second push, so PULLTYPE's LDR
                 * TYPE,[SP],#4 must find the 4 at the lower word
                 * (Funct.s's STR IACC then STR TYPE, both #-4) */
                ros_st32(r13 + 4, r0);      /* the pointer above it */
                goto FNARG3;
            }
            raise_at(s, SITE_ARGMATARR);
        }
        /* a RETURN: an l-value. It is evaluated, and also kept for
         * the end. */
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[7] = r7; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_LVBLNK(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
        if (s->z) {
            if (s->c)
                raise_at(s, SITE_ARGMATRET);/* silly: mismatch */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_CREATE(s);        /* creatable: make it */
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r8 = s->r[8]; r9 = s->r[9];
            r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
        }
        r6 = ros_ld32(r13);
        r7 = ros_ld32(r13 + 4);
        r13 += 8;                           /* LDMFD's writeback */
        r13 -= 8;
        ros_st32(r13, r0);                  /* the l-value */
        ros_st32(r13 + 4, r9);
        TR("lvpush", r13, r0);
        s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_VARIND(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_PUSHTYPE(s);          /* its value */
        r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4];
        r13 = s->r[13]; r14 = s->r[14];
        r13 -= 4;
        ros_st32(r13, s->r[9]);
        TR("valpush", r13, s->r[9]);
        s->r[13] = r13;
FNARG3: /* the LVBLNK paths: the next char is unread */
        do {
            r10 = ros_ld8(r11);
            r11 += 1;
        } while (r10 == 32);
FNARG4: /* the EXPR paths: the lookahead is already in R10 */
        ros_subs(s, r10, 44);
        if (r10 == 44) {                    /* the next, hard from here */
            r4 = ros_ld32(r6);
            r5 = ros_ld32(r6 + 4);
            r6 += 8;
            TR("next-arg", r4, r5);
            if (r4 == 0)
                raise_at(s, SITE_ARGMAT);
            r13 -= 8;
            ros_st32(r13, r6);
            ros_st32(r13 + 4, r7);
            s->r[13] = r13;
            if (r5 & 0x100)
                continue;                   /* FNARGA again */
            s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
            s->r[7] = r7; s->r[9] = r9; s->r[10] = r10;
            s->r[11] = r11; s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_EXPR(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r8 = s->r[8]; r9 = s->r[9];
            r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            r6 = ros_ld32(r13);
            r7 = ros_ld32(r13 + 4);
            r13 += 8;
            s->r[13] = r13;
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_PUSHTYPE(s);
            r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4];
            r13 = s->r[13]; r14 = s->r[14];
            r13 -= 4;
            ros_st32(r13, s->r[9]);
            s->r[13] = r13;
            goto FNARG4;      /* EXPR left the lookahead read */
        }
        ros_subs(s, r10, 41);
        r5 = ros_ld32(r6);
        TR("endchk", r10, r5);
        if (r10 == 41)
            ros_logic(s, r5, s->c);
        if (r10 != 41 || r5 != 0)
            raise_at(s, SITE_ARGMAT);

        /* FNHARDARGZ: assign. RETURNs go through the temporary stack. */
        {
            uint32_t fnloc;
            r10 = r13 - 0x100;              /* SP-256: the temp stack */
            ros_st32(r8 - 404, r10);        /* FNTEMPLOC */
            TR("hz-start", r13, 0);
            do {
                s->r[13] = r13;
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_hand_PULLTYPE(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r9 = s->r[9]; r13 = s->r[13]; r14 = s->r[14];
                r6 -= 8;
                r4 = ros_ld32(r6);
                r5 = ros_ld32(r6 + 4);
                r13 -= 12;
                ros_st32(r13, r5);
                ros_st32(r13 + 4, r6);
                ros_st32(r13 + 8, r7);
                r5 >>= 16;
                s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
                s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_STOREA(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
                r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
                r14 = s->r[14];
                {
                    uint32_t r5s = ros_ld32(r13);
                    r6 = ros_ld32(r13 + 4);
                    r7 = ros_ld32(r13 + 8);
                    if (r5s & 0x400) {      /* a RETURN destination */
                        r4 = ros_ld32(r13 + 12);
                        r5 = ros_ld32(r13 + 16);
                        r13 += 20;
                        ros_st32(r10, r4);  /* to the temporary stack */
                        ros_st32(r10 + 4, r5);
                        r10 += 8;
                        TR("xfer", r13, r10);
                    } else {
                        r5 = r5s;
                        r13 += 12;
                        TR("noxfer", r13, r5s);
                    }
                }
                ros_subs(s, r6, r7);
            } while (r6 != r7);
            r4 = ros_ld32(r8 - 404);        /* FNTEMPLOC */
            fnloc = r10;                    /* its moving top */
            TR("hz-end", r13, r10);
            r10 = ros_ld32(r13 + 4);        /* the saved FN/PROC */
            r0 = ros_ld32(r13) - 8;
            r13 += 8;
            if (r4 == fnloc)
                goto DOFN;                  /* no RETURNs after all */
            r6 = ros_ld32(r13);             /* the list start and flag */
            r7 = ros_ld32(r13 + 4);
            r13 += 8;
            { /* FNPUSHRETURN: the destinations, then the marker */
                uint32_t p = fnloc;
                do {
                    p -= 8;
                    r13 -= 8;
                    ros_st32(r13, ros_ld32(p));
                    ros_st32(r13 + 4, ros_ld32(p + 4));
                } while (p != r4);
                r13 -= 8;
                ros_st32(r13, r6 & 0xBFFFFFFFu);
                ros_st32(r13 + 4, r7);
            }
            TR("pushret", r13, r10);
            goto DOFN;
        }
    }

DOFN: /* the call itself */
    TR("DOFN", r13, r10);
    
    r4 = ros_ld32(r0);                      /* the info triple */
    r5 = ros_ld32(r0 + 4);
    r6 = ros_ld32(r0 + 8);
    ros_logic(s, r4, s->c);
    if (r4 != 0)
        raise_at(s, SITE_ARGMAT);           /* the DEF takes no args */
    r13 -= 12;
    ros_st32(r13, r10);
    ros_st32(r13 + 4, r11);
    ros_st32(r13 + 8, r12);
    r12 = r5;                               /* LINE := the DEF address */
    ros_adds(s, r6, 1);
    s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[15] = s->r[14];
    if (r6 == 0xFFFFFFFF) {
        basicvfp_hand_STMT(s);
        return;
    }
    /* fall through */

DOFNOVERLAY:
    r7 = ros_ld32(r8 - 88);                 /* OVERPTR */
    ros_subs(s, r7, 0);
    s->r[7] = r7; s->r[13] = r13; s->r[15] = s->r[14];
    if ((int32_t)r7 <= 0) {
        basicvfp_ERRQ1(s);
        return;
    }
    r4 = ros_ld32(r7 + 4);
    ros_subs(s, r4, r6);
    s->r[4] = r4; s->r[13] = r13; s->r[15] = s->r[14];
    if (r4 == r6) {
        basicvfp_hand_STMT(s);              /* already the right one */
        return;
    }
    r1 = ros_ld32(r7);
    r0 = r1 + r6 + (r6 << 2);
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARSTR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r14 = s->r[14];
    r4 = r2 - r8;
    r4 = ros_adds(s, r4, 1536);
    if (r4 == 0) {
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_FNMISS(s);
        return;
    }
    ros_st8(r2, 13);
    r2 = r7 + 12;
    s->r[0] = 13; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_OSFILELOADSTRACC(s);           /* load the overlay */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLUSHCACHE(s, &r0, &r1, &r2, r8, s->r[14]);
    ros_st32(r7 + 4, r6);                   /* the current overlay */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[7] = r7;
    s->r[13] = r13; s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
}
