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
 * (Sources/Programmer/BASIC: s.Basic).
 */

/* basic_stmt.c: the head of the statement loop, translated by hand
 * from Basic.s (RISC OS 5.31's BASIC, the VFP build).  It holds STMT,
 * which every statement in every program enters, and CRLINE, which
 * carries the loop from one line to the next.
 *
 * STMT does one instruction's worth of work.  It reads the token and
 * goes on to the dispatch.  It is still the most-executed label in the
 * interpreter.
 *
 * CRLINE is the line-break check.  It skips the line number, which is
 * three bytes in the tokenised form.  Then one of three things
 * happens:
 *   - The program has ended (&FF), and CLRSTK unwinds it.
 *   - Nothing exceptional is pending (ESCWORD is zero), and the next
 *     statement runs at once.
 *   - The exception handler runs between statements, for Escape,
 *     TRACE and the BREAK-like events.  Then the next statement runs.
 * See Basic.s:145-152, the head of the DISPAT sample.
 *
 * The quirks kept, with their lines:
 *   - The line number is skipped BLINDLY: three bytes, whatever they
 *     are (LDRB [LINE],#3).  A mistokenised line misparses exactly as
 *     the original lets it.
 *   - ESCWORD is read again from the workspace on every line and is
 *     never cached.  An Escape set by a SWI is noticed at the next
 *     statement, as on RISC OS.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "basic_stmt.h"

/* The lift's dispatch and unwinding, exported by the twin. */
void basicvfp_DISPAT(struct ros_cpu *s);
void basicvfp_CLRSTK(struct ros_cpu *s);
void basicvfp_DOEXCEPTION(struct ros_cpu *s);

/* STMT (Basic.s:141): the next token, then dispatch. */
void basicvfp_hand_STMT(struct ros_cpu *s)
{
    s->r[10] = ros_ld8(s->r[12]);
    s->r[12] += 1;
    s->r[15] = s->r[14];
    basicvfp_DISPAT(s);
}

/* CRLINE (Basic.s:145): the line break between statements. */
void basicvfp_hand_CRLINE(struct ros_cpu *s)
{
    uint32_t r4, r10, r12 = s->r[12], r8 = s->r[8];

    r10 = ros_ld8(r12);                     /* LDRB R10,[LINE],#3 */
    r12 += 3;
    ros_subs(s, r10, 0xFFu);                /* CMP R10,#&FF */
    if (r10 == 0xFFu) {                     /* the program's end mark */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_CLRSTK(s);
        return;
    }
    r4 = ros_ld32(r8 - 112);                /* ESCWORD */
    ros_subs(s, r4, 0);
    if (r4 == 0) {                          /* nothing exceptional */
        s->r[4] = r4;
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    s->r[4] = r4;                           /* BL DOEXCEPTION */
    s->r[10] = r10;
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_DOEXCEPTION(s);
    /* then on to STMT, as the original's B STMT does */
    basicvfp_hand_STMT(s);
}
