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
 * (Sources/Programmer/BASIC: s.Lexical).
 */

/* lexical_match.h: BASIC's tokeniser, translated by hand
 * (units/lexical_match.c).  These are the three entries the
 * interpreter calls.  The machine's state travels in the register
 * block, as the header comment of Lexical.s documents (SOURCE R1,
 * DEST R2, MODE R3, CONSTA R4, SMODE R5).  Each entry returns through
 * the address it pushed.
 */
#ifndef LEXICAL_MATCH_H
#define LEXICAL_MATCH_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_MATCH(struct ros_cpu *s);
void basicvfp_hand_EVMATCH(struct ros_cpu *s);
void basicvfp_hand_AUMATCH(struct ros_cpu *s);

#endif
