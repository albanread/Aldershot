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
 * (Sources/Programmer/BASIC: s.ErrorMsgs, s.Factor, s.fp, s.fp2).
 */

/* factor_trans.h: BASIC's transcendental functions, translated by hand
 * (units/factor_trans.c).  The interpreter calling convention is unit
 * 0's (basicvfp_hand.h).  On entry the argument is at AELINE and the
 * lookahead is in R10.  On exit the value is in FACC (D0), TYPE is
 * float, the condition codes are as promised, and R15 is set to the
 * return address that was handed in.
 */
#ifndef FACTOR_TRANS_H
#define FACTOR_TRANS_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_SIN(struct ros_cpu *s);
void basicvfp_hand_COS(struct ros_cpu *s);
void basicvfp_hand_TAN(struct ros_cpu *s);
void basicvfp_hand_ASN(struct ros_cpu *s);
void basicvfp_hand_ACS(struct ros_cpu *s);
void basicvfp_hand_ATN(struct ros_cpu *s);
void basicvfp_hand_LN(struct ros_cpu *s);
void basicvfp_hand_LOG(struct ros_cpu *s);
void basicvfp_hand_EXP(struct ros_cpu *s);
void basicvfp_hand_DEG(struct ros_cpu *s);
void basicvfp_hand_RAD(struct ros_cpu *s);

#endif
