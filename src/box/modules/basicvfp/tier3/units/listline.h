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
 * (Sources/Programmer/BASIC: s.Command, s.Lexical, hdr.Tokens).
 */

#ifndef LISTLINE_H
#define LISTLINE_H

#include <stdint.h>

struct ros_cpu;

/* LISTLINE (Command.s:608-659): prints one program line for LIST or
 * the printer.  It is the formatter driven by LISTOP.  It prints the
 * line number, the indent, the statement splits at ':', the
 * line-number constants and the tokens.  It is a whole-function
 * substitution: the lift's basicvfp_LISTLINE becomes a thunk calling
 * this. */
void basicvfp_hand_LISTLINE(struct ros_cpu *s);

#endif
