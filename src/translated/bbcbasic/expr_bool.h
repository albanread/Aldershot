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
 * (Sources/Programmer/BASIC: s.Expr).
 */

#ifndef EXPR_BOOL_H
#define EXPR_BOOL_H

#include <stdint.h>

struct ros_cpu;

void basicvfp_hand_EXPROR(struct ros_cpu *s);
void basicvfp_hand_EXPREOR(struct ros_cpu *s);
void basicvfp_hand_EXPRAND(struct ros_cpu *s);
void basicvfp_hand_EXPRNEQUAL(struct ros_cpu *s);
void basicvfp_hand_EXPRLT(struct ros_cpu *s);
void basicvfp_hand_EXPRLTOREQ(struct ros_cpu *s);
void basicvfp_hand_EXPREQ(struct ros_cpu *s);
void basicvfp_hand_EXPRGT(struct ros_cpu *s);
void basicvfp_hand_EXPRGTOREQ(struct ros_cpu *s);
void basicvfp_hand_EXPRRSHIFT(struct ros_cpu *s);
void basicvfp_hand_EXPRRSHIFTLOGICAL(struct ros_cpu *s);
void basicvfp_hand_EXPRLSHIFT(struct ros_cpu *s);
void basicvfp_hand_EXPRINTDIV(struct ros_cpu *s);
void basicvfp_hand_EXPRMOD(struct ros_cpu *s);

#endif
