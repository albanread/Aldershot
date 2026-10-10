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
 * (Sources/Programmer/BASIC: s.Basic, s.ErrorMsgs).
 */

#ifndef BASIC_ORGANS_H
#define BASIC_ORGANS_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_STORE(struct ros_cpu *s);
void basicvfp_hand_SETVAL(struct ros_cpu *s);
void basicvfp_hand_SETVAR(struct ros_cpu *s, uint32_t *p0, uint32_t r8, uint32_t r14);
void basicvfp_hand_DATA(struct ros_cpu *s);
void basicvfp_hand_NXT(struct ros_cpu *s);
void basicvfp_hand_DONEXT(struct ros_cpu *s);
void basicvfp_hand_DONXTS(struct ros_cpu *s);
void basicvfp_hand_FLUSHCACHE(struct ros_cpu *s, uint32_t *p0, uint32_t *p1, uint32_t *p2, uint32_t r8, uint32_t r14);
void basicvfp_hand_PURGECACHE(struct ros_cpu *s, uint32_t r4, uint32_t *p5, uint32_t *p6, uint32_t *p7, uint32_t r8, uint32_t *p10, uint32_t r11, uint32_t r13, uint32_t *p14);
void basicvfp_hand_MSG(struct ros_cpu *s);
void basicvfp_hand_SETFSA(struct ros_cpu *s);
void basicvfp_hand_INLINE(struct ros_cpu *s);
void basicvfp_hand_TITLE(struct ros_cpu *s);
void basicvfp_hand_ORDERR(struct ros_cpu *s, uint32_t *p0, uint32_t r8, uint32_t r14);

#endif
