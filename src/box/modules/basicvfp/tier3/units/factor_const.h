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
 * (Sources/Programmer/BASIC: s.fp2).
 */

#ifndef FACTOR_CONST_H
#define FACTOR_CONST_H

#include <stdint.h>

struct ros_cpu;

/* FREAD keeps the lift's own signature.  The lifter merged its live
 * registers into pointer parameters.  The twin's thunk passes them in
 * exactly that form, and so do the lift's own call sites, FACTOR's
 * TSTNNOTCACHE and the VAL family. */
void basicvfp_hand_FREAD(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                         uint32_t *p3, uint32_t *p5, uint32_t *p7,
                         uint32_t *p9, uint32_t *p10, uint32_t *p11,
                         uint32_t r12, uint32_t *p13, uint32_t r14);
void basicvfp_hand_FCONFP(struct ros_cpu *s);

#endif
