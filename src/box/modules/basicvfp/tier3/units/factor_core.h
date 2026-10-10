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
 * (Sources/Programmer/BASIC: s.ErrorMsgs, s.Factor, s.fp2, hdr.Definitions).
 */

#ifndef FACTOR_CORE_H
#define FACTOR_CORE_H

#include <stdint.h>

struct ros_cpu;

/* FACTOR's constant readers, in the rig's one-argument case shape.
 * The substituted dispatch case stores every live local into R[], with
 * R[14] = the live return address, and then tail-calls.  So each
 * function reads its state from R[].  Where the original does
 * `MOV PC,R14' (or pops the address from the arena stack), it returns
 * with R[15] = R[14].
 *
 * TSTN owns the eleven dispatch cases '.' and '0'-'9'.  HEXIN is '&'
 * (case 38), BININ is '%' (case 37) and UNMINS is '-' (case 45). */
void basicvfp_hand_TSTN(struct ros_cpu *s);
void basicvfp_hand_HEXIN(struct ros_cpu *s);
void basicvfp_hand_BININ(struct ros_cpu *s);
void basicvfp_hand_UNMINS(struct ros_cpu *s);

#endif
