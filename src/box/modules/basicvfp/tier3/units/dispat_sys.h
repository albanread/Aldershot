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
 * (Sources/Programmer/BASIC: s.Basic, s.Funct, s.Stmt, s.Stmt2, hdr.Definitions, hdr.Tokens).
 */

/* dispat_sys.h: the system statements of the statement executor,
 * translated by hand (units/dispat_sys.c).
 *
 * In the lift, as in the original, these are labels inside the one
 * DISPAT, and the token switch enters them. The rig's case
 * substitution calls each function here with the live return address
 * in R14. This is the one-argument ROS_TAIL_CALL contract that every
 * CASE_SUBS hand function follows. Statements never return. Their
 * exits continue into STMT, CRLINE, DATA, NXT, DONEXT, DONXTS, DISPAT
 * or an error.
 *
 * The entry protocol:
 *   - R10 holds the current token
 *   - R12 is LINE, just past the token
 *   - R8 is ARGP and R13 is SP
 *   - the other registers hold whatever the previous statement left.
 *     The substituted case stores every live local first.
 * The dispatch case numbers are in the first switch unless noted:
 *   - DOSTAR 42
 *   - FNRET 61 (=, the statement that returns from an FN)
 *   - OTHER 127 and 201. The ObjAsm WHEN is an alias of OTHER, so the
 *     WHEN and OTHERWISE arms share one body (Stmt.s:127-129).
 *   - LERROR 133, BBPUT 213, CALL 214, ENVEL 226, REPORT 246,
 *     WIDTH 254, OSCL 255.
 * In the second switch, for escape tokens (after TWOSTMT's jump
 * table), they are ORGIN 25820, DOMOUSE 25152, SYS 27632,
 * INSTALLBAD 61076, LIBRARY 27288 and OVERLAY 26436.
 *
 * TWOSTMT (case 200) is not here. It is the trunk of the second
 * switch. It reads the extension byte, checks it against
 * TTWOSTMTLIMIT (&A4), and jumps through the second table. Its
 * targets are labels inside DISPAT that other groups own. Group C's
 * CIRCLE, FILL, PSET, RECT, DOTINT, ELLIPSE, BEATS, TEMPO, VOICES,
 * VOICE and STEREO are still lift code, and a hand-written trunk
 * could not reach them. The trunk itself is three instructions and a
 * table read (Basic.s:1161-1169). It stays as lift code, and its
 * cases are substituted one statement at a time, exactly as SWAP,
 * WHILE, QUIT and CASE already are.
 */

#ifndef DISPAT_SYS_H
#define DISPAT_SYS_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_DOSTAR(struct ros_cpu *s);
void basicvfp_hand_OSCL(struct ros_cpu *s);
void basicvfp_hand_SYS(struct ros_cpu *s);
void basicvfp_hand_CALL(struct ros_cpu *s);
void basicvfp_hand_LIBRARY(struct ros_cpu *s);
void basicvfp_hand_INSTALLBAD(struct ros_cpu *s);
void basicvfp_hand_OVERLAY(struct ros_cpu *s);
void basicvfp_hand_ENVEL(struct ros_cpu *s);
void basicvfp_hand_REPORT(struct ros_cpu *s);
void basicvfp_hand_LERROR(struct ros_cpu *s);
void basicvfp_hand_OTHER(struct ros_cpu *s);
void basicvfp_hand_WIDTH(struct ros_cpu *s);
void basicvfp_hand_ORGIN(struct ros_cpu *s);
void basicvfp_hand_DOMOUSE(struct ros_cpu *s);
void basicvfp_hand_BBPUT(struct ros_cpu *s);
void basicvfp_hand_FNRET(struct ros_cpu *s);

#endif
