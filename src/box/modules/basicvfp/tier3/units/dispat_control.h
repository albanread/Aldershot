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

/* dispat_control.h: the bodies of the control-flow statements of the
 * statement executor, translated by hand (units/dispat_control.c).
 *
 * None of these bodies is a lifter function. In the lift they are
 * labels inside the one basicvfp_DISPAT, and the token switch enters
 * them (`case N: goto LABEL;`). The integration replaces the arms of
 * that switch with calls to these functions, in the rig's
 * case-substitution shape:
 *
 *     case N: { R[10] = r10; R[11] = r11; R[13] = r13;
 *               basicvfp_hand_<LABEL>(s, r14); return; }
 *
 * So each function receives the live return address, which is what
 * `STR R14` would have pushed. Like every statement body in the
 * original, it never comes back. Its exits continue into STMT,
 * CRLINE, DATA, NXT, DONEXT, DONXTS, DISPAT or an error, as plain
 * calls. This is the landed convention: a call whose result is
 * discarded and which returns at once. The compiler turns it back
 * into the original's branch.
 *
 * The entry protocol is the lift's, read from each body:
 *   - R10 holds the current token
 *   - R12 is LINE, just past the token
 *   - R8 is ARGP and R13 is SP
 *   - the other registers hold whatever the previous statement left.
 * The dispatch case numbers are in the first switch unless noted:
 * ELSE2 204, ENDWH 206, CHAIN 215, END 224, ENDPR 225, FOR 227,
 * GOSUB 228, GOTO 229, IF 231, NEXT 237, ON 238, REPEAT 245,
 * RETURN 248, RUN 249, STOP 250, TRACE 252, UNTIL 253. In the second
 * switch, for escape tokens (after TWOSTMT), they are CASE 15528,
 * WHILE 21544 and QUIT 28456.
 */

#ifndef DISPAT_CONTROL_H
#define DISPAT_CONTROL_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_IF(struct ros_cpu *s);
void basicvfp_hand_ELSE2(struct ros_cpu *s);
void basicvfp_hand_FOR(struct ros_cpu *s);
void basicvfp_hand_NEXT(struct ros_cpu *s);
void basicvfp_hand_WHILE(struct ros_cpu *s);
void basicvfp_hand_ENDWH(struct ros_cpu *s);
void basicvfp_hand_REPEAT(struct ros_cpu *s);
void basicvfp_hand_UNTIL(struct ros_cpu *s);
void basicvfp_hand_CASE(struct ros_cpu *s);
void basicvfp_hand_ON(struct ros_cpu *s);
void basicvfp_hand_GOTO(struct ros_cpu *s);
void basicvfp_hand_GOSUB(struct ros_cpu *s);
void basicvfp_hand_RETURN(struct ros_cpu *s);
void basicvfp_hand_END(struct ros_cpu *s);
void basicvfp_hand_ENDPR(struct ros_cpu *s);
void basicvfp_hand_STOP(struct ros_cpu *s);
void basicvfp_hand_TRACE(struct ros_cpu *s);
void basicvfp_hand_QUIT(struct ros_cpu *s);
void basicvfp_hand_CHAIN(struct ros_cpu *s);
void basicvfp_hand_RUN(struct ros_cpu *s);

void basicvfp_hand_CURSON(struct ros_cpu *s, uint32_t ret_to);
void basicvfp_hand_CURSOFF(struct ros_cpu *s);

#endif
