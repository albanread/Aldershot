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
 * (Sources/Programmer/BASIC: s.Stmt, s.Stmt2, hdr.Definitions, hdr.Tokens).
 */

/* dispat_io.h: the I/O and graphics statements of the statement
 * executor, translated by hand (units/dispat_io.c).
 *
 * In the lift, as in the original, these are labels inside the one
 * DISPAT, entered by the token switch.  The rig's case substitution
 * calls each function here through the one-argument ROS_TAIL_CALL
 * contract that every CASE_SUBS hand function follows.  Statements
 * never return.  Their exits tail into STMT, CRLINE, NXT, DONEXT,
 * DONXTS or an error.  The live return address is in R[14], which the
 * case stores after every live local.
 *
 * Entry protocol: R10 holds the current token and R12 (LINE) points
 * just past it.  R8 is ARGP and R13 is SP.  The other registers hold
 * whatever the previous statement left.
 *
 * The dispatch case numbers are in the first switch unless noted:
 * LINEST 134, CURSOFF 135, SOUND 212, CLEAR 216, CLG 218, CLS 219,
 * DRAW 223, GCOL 230, INPUT 232, MODES 235, MOVE 236, VDU 239, PLOT
 * 240, PRINT 241, READ 243, RESTORE 247, COLOUR 251.  These are in the
 * escape-token second switch, after TWOSTMT's jump table: BEATS 20684,
 * VOICES 20696, VOICE 20724, STEREO 20784, TEMPO 20772, ELLIPSE 23756,
 * CIRCLE 23000, FILL 24116, PSET 25908, RECT 25976, DOTINT 26228,
 * WAIT 26372.  Every label owns exactly one case, so there are no
 * lists.
 *
 * CURSOFF (case 135) is NOT defined here.  Its body already lives in
 * dispat_control.c as basicvfp_hand_CURSOFF.  Before its case can
 * land, that function must be brought to the one-argument CASE_SUBS
 * shape.  This means dropping its unused ret_to parameter in both
 * dispat_control.c and dispat_control.h.  The body is already the
 * case's, and it never returns.  Then 'CURSOFF': 135 joins CASE_SUBS.
 * Until then the case stays in the lift, which behaves correctly.
 */

#ifndef DISPAT_IO_H
#define DISPAT_IO_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_PRINT(struct ros_cpu *s);
void basicvfp_hand_VDU(struct ros_cpu *s);
void basicvfp_hand_INPUT(struct ros_cpu *s);
void basicvfp_hand_LINEST(struct ros_cpu *s);
void basicvfp_hand_READ(struct ros_cpu *s);
void basicvfp_hand_RESTORE(struct ros_cpu *s);
void basicvfp_hand_SOUND(struct ros_cpu *s);
void basicvfp_hand_STEREO(struct ros_cpu *s);
void basicvfp_hand_VOICE(struct ros_cpu *s);
void basicvfp_hand_VOICES(struct ros_cpu *s);
void basicvfp_hand_BEATS(struct ros_cpu *s);
void basicvfp_hand_TEMPO(struct ros_cpu *s);
void basicvfp_hand_MODES(struct ros_cpu *s);
void basicvfp_hand_COLOUR(struct ros_cpu *s);
void basicvfp_hand_GCOL(struct ros_cpu *s);
void basicvfp_hand_MOVE(struct ros_cpu *s);
void basicvfp_hand_DRAW(struct ros_cpu *s);
void basicvfp_hand_CIRCLE(struct ros_cpu *s);
void basicvfp_hand_ELLIPSE(struct ros_cpu *s);
void basicvfp_hand_FILL(struct ros_cpu *s);
void basicvfp_hand_PLOT(struct ros_cpu *s);
void basicvfp_hand_RECT(struct ros_cpu *s);
void basicvfp_hand_DOTINT(struct ros_cpu *s);
void basicvfp_hand_PSET(struct ros_cpu *s);
void basicvfp_hand_CLS(struct ros_cpu *s);
void basicvfp_hand_CLG(struct ros_cpu *s);
void basicvfp_hand_CLEAR(struct ros_cpu *s);
void basicvfp_hand_WAIT(struct ros_cpu *s);

#endif
