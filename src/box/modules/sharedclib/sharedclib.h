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
 * This file is a reimplementation in C of RISC OS Open's SharedCLibrary module
 * (Sources/Lib/RISC_OSLib: clib/s/cl_rmhdr, s/initmodule, s/h_brazil).
 */

/* sharedclib.h: the SharedCLibrary module (RISC_OSLib 6.23), reimplemented
 * natively over the ROM C library's image (sharedclib.c). */
#ifndef ROSGD_SHAREDCLIB_H
#define ROSGD_SHAREDCLIB_H

#include <stdint.h>

#include "rosgd/module.h"

extern struct ros_module sharedclib_module;

/* The library's version, as registration returns it in R6
 * (RISC_OSLib s/h_brazil, LibraryVersionNumber). */
#define ROS_CLIB_LIBRARY_VERSION 6u

/* The image's header in the C ROM, once the module has mapped and checked
 * it (in the box). Returns 0 if there is none. That is the case in the
 * hosted build, and before the module's first initialisation. */
uint32_t ros_sharedclib_image(void);

#endif
