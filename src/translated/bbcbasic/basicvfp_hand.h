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
 * (Sources/Programmer/BASIC: s.ErrorMsgs, s.Factor, s.fp).
 */

/* basicvfp_hand.h -- BBC BASIC's built-in functions, translated by hand.
 *
 * This is for comparison against the machine translation (rosasm
 * --emit c). These functions replace twelve of the FACTOR dispatch
 * table's entries, one for one. They keep the original's behaviour,
 * including its edge cases, and the quirks are noted at each function.
 * The rest of the interpreter is the machine translation, unchanged, so
 * the same BASIC programs can be run against either and the output
 * compared.
 *
 * These functions follow the interpreter's calling convention:
 *   entry: the argument expression starts at AELINE (R11). The next
 *          character is already in the lookahead, R10.
 *   exit:  the value is in the accumulator. That is IACC (R0) for
 *          integers and string pointers, and FACC (D0) for floats. Its
 *          TYPE is in R9: 0 for a string (bytes at STRACC, length
 *          CLEN-STRACC, CLEN in R2), &40000000 for an integer and
 *          &80000000 for a float. R15 is set to the return address that
 *          was handed in, exactly as `LDR PC,[SP],#4` does.
 */
#ifndef BASICVFP_HAND_H
#define BASICVFP_HAND_H

#include <stdint.h>

struct ros_cpu;

/* The twelve, named as the source names them (Factor.s). */
void basicvfp_hand_ABS(struct ros_cpu *s);
void basicvfp_hand_SGN(struct ros_cpu *s);
void basicvfp_hand_INT(struct ros_cpu *s);
void basicvfp_hand_SQR(struct ros_cpu *s);
void basicvfp_hand_LEN(struct ros_cpu *s);
void basicvfp_hand_ASC(struct ros_cpu *s);
void basicvfp_hand_INSTR(struct ros_cpu *s);
void basicvfp_hand_CHRD(struct ros_cpu *s);
void basicvfp_hand_LEFTD(struct ros_cpu *s);
void basicvfp_hand_RIGHTD(struct ros_cpu *s);
void basicvfp_hand_MIDD(struct ros_cpu *s);
void basicvfp_hand_STRND(struct ros_cpu *s);

#endif
