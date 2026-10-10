/* Copyright 1996 Acorn Computers Ltd
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
 * This file is a reimplementation in C of RISC OS Open's SpriteUtils module
 * (Sources/Video/Render/SpriteUtil: s.SpriteUtil, s.MsgCode).
 */

/* spriteutils.h: SpriteUtils, a native module that provides the system
 * sprite area's star commands (see README.md). It has no SWIs.
 * Its C code calls OS_SpriteOp itself. */
#ifndef ROSGD_SPRITEUTILS_H
#define ROSGD_SPRITEUTILS_H

/* The module, for the ROM's list of native modules. */
extern struct ros_module spriteutils_module;

#endif
