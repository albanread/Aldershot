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
 * (Sources/Programmer/BASIC: s.Basic, s.Expr).
 */

/* basic_lookup.c: BASIC's variable lookup walk, translated by hand
 * from Basic.s (RISC OS 5.31's BASIC, the VFP build).  It holds LOOKUP
 * through LOOKP1A, the create path (GOTLTCREATE/CREATE), and AELV.
 *
 * Variables live in one linked list per first letter, hung from
 * VARPTR at ARGP-&200.  Each entry is a link word followed by the name
 * as bytes.  The first two bytes are packed big-endian into one word.
 * This lets the walk test a name's first character with one shift and
 * compare (LOOKP1A's TEQ R3,R5,LSR #24).
 *
 * The walk returns with R0 past the matched name and R1 holding the
 * next link.  The search's answer is in C, which is clear only when
 * the name matched to its end at AELINE.  LOOKFL tests R1 against 1,
 * the "create me" marker, and the callers read the flags the walk
 * left.
 *
 * GOTLTCREATE (Expr.s:56) is called from the assignment path,
 * LETSTNOTCACHE (Basic.s:858), when LVNOTCACHE did not find the
 * variable.  The flags are still those LVNOTCACHE left.  The first
 * thing GOTLTCREATE does is branch on C, so nothing here may touch
 * the flags before that test.
 *
 * The quirks kept, with their lines:
 *   - A single-character entry whose name in the line also has one
 *     character matches with R0 wound back two, where three would be
 *     expected (LOOKP1A's SUBEQ R0,R0,#2).  The entry's terminator is
 *     not consumed.
 *   - The byte loop compares up to and including the entry's
 *     terminating NUL.  Only then does it check that the line's name
 *     ended too.
 *   - CREATE hands CREALP the address of the VARPTR slot, which is
 *     the letter's own list.  It does not hand it the list head.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "basic_lookup.h"

/* The lift's routines the family hands off to. */
void basicvfp_DONEXT(struct ros_cpu *s);
void basicvfp_MISTAK(struct ros_cpu *s);
void basicvfp_CREALP(struct ros_cpu *s);
void basicvfp_LVBLNK(struct ros_cpu *s);

/* LOOKUP: find the list for R10's letter.  If it has entries, walk
 * it.  If it is empty, go to LOOKFL. */
void basicvfp_hand_LOOKUP(struct ros_cpu *s)
{
    uint32_t r8 = s->r[8], r10 = s->r[10];
    uint32_t r1 = ros_ld32(r8 - 0x200u + ((r10 - 64) << 2));
    s->r[0] = r1;
    s->r[1] = r1;
    if (r1 != 0)
        basicvfp_hand_LOOKP1A(s);
    else
        basicvfp_hand_LOOKFL(s);
}

/* LOOKFL: the answer for an empty list, which is the flags of
 * TEQ R1,#1. */
void basicvfp_hand_LOOKFL(struct ros_cpu *s)
{
    ros_logic(s, s->r[1] ^ 1, s->c);        /* TEQ R1,#1 */
    s->r[15] = s->r[14];
}

/* LOOKP1: the next link, or the end of the list. */
void basicvfp_hand_LOOKP1(struct ros_cpu *s)
{
    uint32_t r0 = s->r[1];
    s->r[0] = r0;
    if (r0 == 0)
        basicvfp_hand_LOOKFL(s);
    else
        basicvfp_hand_LOOKP1A(s);
}

/* LOOKP1A: one entry against the name at R4, ending at AELINE (R11);
 * R3 is the name's first character. */
void basicvfp_hand_LOOKP1A(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5, r6 = s->r[6], r11 = s->r[11];

    r1 = ros_ld32(r0);                      /* LDMIA R0!,{R1,R5} */
    r5 = ros_ror(ros_ld32(r0 + 4), 8);
    r0 += 8;
    if (r3 ^ (r5 >> 24)) {                  /* first character */
        s->r[0] = r0; s->r[1] = r1; s->r[5] = r5;
        basicvfp_hand_LOOKP1(s);
        return;
    }
    r5 &= 0xFFu;                            /* the entry's second */
    if (r5 == 0) {                          /* a one-character entry */
        ros_logic(s, r4 ^ r11, s->c);       /* TEQ R4,AELINE */
        if (s->z)
            r0 -= 2;                        /* the terminator stays */
        if (s->z) {
            s->r[0] = r0; s->r[1] = r1; s->r[5] = r5;
            s->r[15] = s->r[14];
            return;
        }
        s->r[0] = r0; s->r[1] = r1; s->r[5] = r5;
        basicvfp_hand_LOOKP1(s);
        return;
    }
    if (r4 == r11) {                        /* the line's name ended */
        s->r[0] = r0; s->r[1] = r1; s->r[5] = r5;
        basicvfp_hand_LOOKP1(s);
        return;
    }
    r6 = ros_ld8(r4);                       /* the line's second */
    if (r6 ^ r5) {
        s->r[0] = r0; s->r[1] = r1; s->r[5] = r5; s->r[6] = r6;
        basicvfp_hand_LOOKP1(s);
        return;
    }
    r2 = r4;                                /* compare the rest */
    r0 -= 2;
    do {
        r2 += 1;
        r6 = ros_ld8(r2);
        r5 = ros_ld8(r0);
        r0 += 1;
    } while (r6 == r5);
    if (r5 != 0) {                          /* the entry is longer */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[5] = r5;
        s->r[6] = r6;
        basicvfp_hand_LOOKP1(s);
        return;
    }
    ros_logic(s, r2 ^ r11, s->c);           /* TEQ R2,AELINE */
    if (!s->z) {                            /* the line is longer */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[5] = r5;
        s->r[6] = r6;
        basicvfp_hand_LOOKP1(s);
        return;
    }
    s->r[0] = r0;                           /* the match */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[15] = s->r[14];
}

/* GOTLTCREATE: an assignment to a variable that does not exist yet.
 * With C set, control goes straight to DONEXT (the original's BCS
 * DONEXT, Expr.s:57).  Otherwise the next character after any spaces
 * must be "=", and the variable is created.  Anything else is MISTAK. */
void basicvfp_hand_GOTLTCREATE(struct ros_cpu *s)
{
    uint32_t r0, r7;
    if (s->c) {                             /* BCS DONEXT */
        s->r[15] = s->r[14];
        basicvfp_DONEXT(s);
        return;
    }
    r0 = s->r[11];                          /* AELINE */
    do {
        r7 = ros_ld8(r0);
        r0 += 1;
    } while (r7 == 32);
    ros_subs(s, r7, 61);                    /* CMP R7,#"=" */
    if (r7 != 61) {
        s->r[0] = r0; s->r[7] = r7;
        s->r[15] = s->r[14];
        basicvfp_MISTAK(s);
        return;
    }
    s->r[0] = r0;
    s->r[7] = r7;
    s->r[15] = s->r[14];
    basicvfp_hand_CREATE(s);
}

/* CREATE: the VARPTR slot for R10's letter, then CREALP. */
void basicvfp_hand_CREATE(struct ros_cpu *s)
{
    s->r[0] = s->r[8] + ((s->r[10] - 64) << 2) - 0x200u;
    s->r[15] = s->r[14];
    basicvfp_CREALP(s);
}

/* AELV: the line becomes the expression line, then LVBLNK. */
void basicvfp_hand_AELV(struct ros_cpu *s)
{
    s->r[11] = s->r[12];
    s->r[15] = s->r[14];
    basicvfp_LVBLNK(s);
}
