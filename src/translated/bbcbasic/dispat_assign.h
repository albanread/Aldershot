/* Copyright 2001 Pace Micro Technology plc
 * Copyright 2009 Castle Technology Ltd
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
 * (Sources/Programmer/BASIC: s.Array, s.Basic, s.Stmt, s.Stmt2, hdr.Definitions, hdr.Tokens).
 */

/* dispat_assign.h: the assignment and LET family of the statement
 * executor, translated by hand (units/dispat_assign.c).
 *
 * In the lift, as in the original, these are labels inside the one
 * DISPAT, and the token switch enters them.  The rig's case
 * substitution calls each function here with the live return address
 * in R14.  This is the one-argument ROS_TAIL_CALL contract that every
 * CASE_SUBS hand function follows.  Statements never return.  Each
 * exit is a tail call into STMT, CRLINE, DATA, NXT, DONEXT, DONXTS,
 * DISPAT or an error.
 *
 * Entry protocol: R10 holds the current token, R12 (LINE) points just
 * past it, R8 is ARGP and R13 is SP.  The other registers hold
 * whatever the previous statement left.  The substituted case stores
 * every live local first.
 *
 * The dispatch case numbers are in the first switch unless noted:
 *   - LETST is the letter range 37-41,43-57,59,60,62,65-90,92-122.
 *     One case number is not enough, so the rig's CASE_SUBS needs a
 *     list form for it.
 *   - LETSTNOTCACHE 33, 36, 63, 124.
 *   - ASSIGNAT 64, LEXT 162, LLEFTD 192, LMIDD 193, LRIGHTD 194,
 *     LET 233, DIM 222, LOCAL 234.
 *   - The LVALUE keywords have TWO tokens each, the plain form and the
 *     statement form: LPTR 143 and 207, LPAGE 144 and 208, LTIME 145
 *     and 209, LLOMEM 146 and 210, LHIMEM 147 and 211.
 *   - SWAP is 20988, in the second switch for escape tokens (after
 *     TWOSTMT's jump table).
 */

#ifndef DISPAT_ASSIGN_H
#define DISPAT_ASSIGN_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_LET(struct ros_cpu *s);
void basicvfp_hand_LETST(struct ros_cpu *s);
void basicvfp_hand_LETSTNOTCACHE(struct ros_cpu *s);
void basicvfp_hand_ASSIGNAT(struct ros_cpu *s);
void basicvfp_hand_DIM(struct ros_cpu *s);
void basicvfp_hand_LOCAL(struct ros_cpu *s);
void basicvfp_hand_SWAP(struct ros_cpu *s);
void basicvfp_hand_LEXT(struct ros_cpu *s);
void basicvfp_hand_LLEFTD(struct ros_cpu *s);
void basicvfp_hand_LRIGHTD(struct ros_cpu *s);
void basicvfp_hand_LMIDD(struct ros_cpu *s);
void basicvfp_hand_LLOMEM(struct ros_cpu *s);
void basicvfp_hand_LPAGE(struct ros_cpu *s);
void basicvfp_hand_LPTR(struct ros_cpu *s);
void basicvfp_hand_LTIME(struct ros_cpu *s);
void basicvfp_hand_LHIMEM(struct ros_cpu *s);

#endif
