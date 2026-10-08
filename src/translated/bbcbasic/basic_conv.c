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
 * (Sources/Programmer/BASIC: s.fp).
 */

/* basic_conv.c: BASIC's type conversions, translated by hand from
 * fp.s:14-80 (RISC OS 5.31's BASIC, the VFP build).  These are the
 * FLOATY and INTEGY families.  Every operator and built-in calls them
 * to move a value between the integer accumulator and the float one.
 *
 * In the original, each family is one routine with several entries.
 * FLOATY tests the type and converts an integer to a float.  FLOATZ
 * returns at once for a float.  FLOATQ rejects a string.  INTEGY,
 * INTEGZ and INTEGB do the same in the other direction.  They end in
 * SFIX's FTOSIZD, whose out-of-range result raises an error through
 * the FPSCR.  This is the same path that unit 0's acc_int spells out.
 * The entries are separate labels in the lift, so each one is thunked.
 * Here the chains call each other directly.
 *
 * The condition codes are part of each contract.  The TEQ that tests
 * TYPE leaves flags that the original's conditional returns read.  The
 * callers may read them too after a BL.  So they are set here at the
 * same points.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "basic_conv.h"

/* The lift's error paths. */
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_VFPException(struct ros_cpu *s);

#define T_INTEGER 0x40000000u
#define T_FLOAT 0x80000000u

/* ---- integers to floats (fp.s:14-27, 71-80) ------------------------ */

/* FLOATY: TEQ TYPE,#0, then FLOATZ. */
void basicvfp_hand_FLOATY(struct ros_cpu *s)
{
    ros_logic(s, s->r[9], s->c);            /* TEQ TYPE,#0 */
    basicvfp_hand_FLOATZ(s);
}

/* FLOATZ: return if the value is already a float, else FLOATQ. */
void basicvfp_hand_FLOATZ(struct ros_cpu *s)
{
    if (s->n) {                             /* MOVMI PC,R14 */
        s->r[15] = s->r[14];
        return;
    }
    basicvfp_hand_FLOATQ(s);
}

/* FLOATQ: a string is a type error.  An integer becomes a float. */
void basicvfp_hand_FLOATQ(struct ros_cpu *s)
{
    if (s->z) {                             /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }
    s->r[9] = T_FLOAT;
    basicvfp_hand_IFLT(s);
}

/* IFLT: FSITOD converts the integer accumulator to the float one. */
void basicvfp_hand_IFLT(struct ros_cpu *s)
{
    s->fp->vfp.sw[0] = s->r[0];             /* FMSR S0,IACC */
    s->fp->vfp.d[0] = (double)(int32_t)s->fp->vfp.sw[0]; /* FSITOD */
    s->r[15] = s->r[14];
}

/* ---- floats to integers (fp.s:60-80) -------------------------------- */

/* INTEGY: TEQ TYPE,#0, then INTEGZ. */
void basicvfp_hand_INTEGY(struct ros_cpu *s)
{
    ros_logic(s, s->r[9], s->c);            /* TEQ TYPE,#0 */
    basicvfp_hand_INTEGZ(s);
}

/* INTEGZ: a string is a type error, an integer stays, a float
 * converts. */
void basicvfp_hand_INTEGZ(struct ros_cpu *s)
{
    if (s->z) {                             /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }
    if (!s->n) {                            /* MOVPL PC,R14 */
        s->r[15] = s->r[14];
        return;
    }
    basicvfp_hand_INTEGB(s);
}

/* INTEGB: the type becomes integer, then SFIX. */
void basicvfp_hand_INTEGB(struct ros_cpu *s)
{
    s->r[9] = T_INTEGER;
    basicvfp_hand_SFIX(s);
}

/* SFIX: FTOSIZD truncates toward zero.  A value out of range raises
 * "Invalid arithmetic operation" through the FPSCR (fp.s:78). */
void basicvfp_hand_SFIX(struct ros_cpu *s)
{
    uint32_t r0;
    s->fp->fpscr |= ros_vfp_int_ex(s->fp->vfp.d[0], ROS_ROUND_ZERO, 1);
    s->fp->vfp.sw[0] = (uint32_t)ros_to_int(s->fp->vfp.d[0], ROS_ROUND_ZERO);
    r0 = s->fp->fpscr;                      /* FMRX IACC,FPSCR */
    ros_logic(s, r0 & 7, s->c);             /* TST IACC,#IOC+DZC+OFC */
    if (!s->z) {                            /* BNE VFPException */
        s->r[0] = r0;
        s->r[15] = s->r[14];
        basicvfp_VFPException(s);
    }
    s->r[0] = s->fp->vfp.sw[0];             /* FMRS IACC,S0 */
    s->r[15] = s->r[14];
}
