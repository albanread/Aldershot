/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2020 Julie Stamp
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
 * This file is a reimplementation in C of RISC OS Open's ShellCLI module
 * (Sources/Desktop/ShellCLI: c.module, s.veneer, s.Shell, cmhg.ShellCLIHdr).
 */

/* shellcli.h: the ShellCLI module (shellcli.c). It gives a command line at
 * the bottom of the desktop's screen, reached from the Task Manager's F12
 * key and its *Commands item.
 */
#ifndef ROSGD_SHELLCLI_H
#define ROSGD_SHELLCLI_H

#include <stdint.h>

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module shellcli_module;

/* Its SWI chunk (Global/SWIs, ShellSWI_Base) */
#define SHELLCLI_CHUNK 0x405C0u

/* Its errors (Global/NewErrors: the block from &900, and CantKill) */
#define SHELLCLI_ERR_CREATION   0x900u  /* "NoSpawn": ShellCLI not active */
#define SHELLCLI_ERR_REMOVAL    0x901u  /* "SActive": ShellCLI task is still active */
/* "WActive": Window Manager is in use. This is the Wimp's own WimpCantKill, &104. */
#define SHELLCLI_ERR_WIMP_ACTIVE ROS_ERR_WIMP_CANT_KILL

/* What *ShellCLI says first, from its Messages file ("Greeting") */
#define SHELLCLI_GREETING \
    "This is the ShellCLI. Press Return without entering any text to return to the desktop."

#endif
