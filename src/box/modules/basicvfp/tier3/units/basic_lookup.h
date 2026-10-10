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
 * (Sources/Programmer/BASIC: s.Basic, s.Expr).
 */

#ifndef BASIC_LOOKUP_H
#define BASIC_LOOKUP_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_LOOKUP(struct ros_cpu *s);
void basicvfp_hand_LOOKFL(struct ros_cpu *s);
void basicvfp_hand_LOOKP1(struct ros_cpu *s);
void basicvfp_hand_LOOKP1A(struct ros_cpu *s);
void basicvfp_hand_GOTLTCREATE(struct ros_cpu *s);
void basicvfp_hand_CREATE(struct ros_cpu *s);
void basicvfp_hand_AELV(struct ros_cpu *s);

#endif
