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
 * (Sources/Programmer/BASIC: s.Basic, s.ModHead, hdr.WorkSpace).
 */

#ifndef MODMAIN_H
#define MODMAIN_H

#include <stdint.h>

struct ros_cpu;

/* MODULEMAIN (Basic.s:23-612) is the module's start entry. It checks
 * for a VFP context and creates one, sets up the resource-file tables
 * and the four environment handlers, and puts the workspace in its
 * initial state. It then parses the command tail for a keyword
 * (-help, -load, -quit, -chain, @hex,hex or a plain filename), and ends
 * in FSASET, RUNNER or CLRSTK. This replaces the whole function. The
 * lift's basicvfp_MODULEMAIN becomes a thunk that calls this. */
void basicvfp_hand_MODULEMAIN(struct ros_cpu *s);

#endif
