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
 * (Sources/Programmer/BASIC: s.Basic, s.Command, hdr.Definitions, hdr.Tokens).
 */

#ifndef CLRSTK_H
#define CLRSTK_H

#include <stdint.h>

struct ros_cpu;

/* CLRSTK (Basic.s:619-707 and the whole of Command.s) is the
 * immediate-mode loop. It covers the > prompt, the test for line entry,
 * insertion or renumbering, and the seventeen command bodies that the DC
 * two-star table dispatches to. This replaces the whole function. The
 * lift's basicvfp_CLRSTK becomes a thunk that calls this. */
void basicvfp_hand_CLRSTK(struct ros_cpu *s);

#endif
