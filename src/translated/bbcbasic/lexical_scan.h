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
 * (Sources/Programmer/BASIC: s.Lexical, hdr.Tokens).
 */

/* lexical_scan.h: BASIC's lexical scan helpers, translated by hand
 * (units/lexical_scan.c).  The signatures are the lift's own.  Some
 * registers the lifter could not keep within one function: R10 and
 * R12 for SPACES, and R2, R4 and R5 for CONSTI.  These arrive as
 * pointer parameters and are written back through them.  Everything
 * else is in the register block.  The return is R15 := R14, as
 * `MOV PC,R14` was.
 */
#ifndef LEXICAL_SCAN_H
#define LEXICAL_SCAN_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_SPACES(struct ros_cpu *s, uint32_t *p10, uint32_t *p12,
                          uint32_t r14);
void basicvfp_hand_AESPAC(struct ros_cpu *s);
void basicvfp_hand_DONE(struct ros_cpu *s);
void basicvfp_hand_DONES(struct ros_cpu *s);
void basicvfp_hand_AEDONE(struct ros_cpu *s);
void basicvfp_hand_AEDONES(struct ros_cpu *s);
void basicvfp_hand_SPTSTN(struct ros_cpu *s);
void basicvfp_hand_SPGETN(struct ros_cpu *s);
void basicvfp_hand_OSSTRI(struct ros_cpu *s);
void basicvfp_hand_WORDCQ(struct ros_cpu *s);
void basicvfp_hand_NUMBCP(struct ros_cpu *s);
void basicvfp_hand_NUMBCQ(struct ros_cpu *s);
void basicvfp_hand_CONSTI(struct ros_cpu *s, uint32_t *p2, uint32_t *p4,
                          uint32_t *p5, uint32_t r14);

#endif
