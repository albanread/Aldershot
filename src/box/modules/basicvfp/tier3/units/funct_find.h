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
 * (Sources/Programmer/BASIC: s.Funct).
 */

#ifndef FUNCT_FIND_H
#define FUNCT_FIND_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_FNFIND(struct ros_cpu *s, uint32_t *p0, uint32_t *p2,
                          uint32_t r3, uint32_t r4, uint32_t *p5,
                          uint32_t *p6, uint32_t *p7, uint32_t r10,
                          uint32_t r11, uint32_t *p14);
void basicvfp_hand_FNDEFLIST(struct ros_cpu *s);

#endif
