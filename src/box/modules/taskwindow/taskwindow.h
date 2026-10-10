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
 * This file is a reimplementation in C of RISC OS Open's TaskWindow module
 * (Sources/Desktop/TaskWindow: s.Taskman, s.TaskWindow, s.Messages).
 */

/* taskwindow.h: the TaskWindow module (taskwindow.c).  It runs a command as a
 * Wimp task of its own.  The command's output is sent to its parent as
 * messages, and its input is taken from them.  README.md lists the commands
 * and messages.
 */
#ifndef ROSGD_TASKWINDOW_H
#define ROSGD_TASKWINDOW_H

#include <stdint.h>

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module taskwindow_module;

/* The TaskWindow protocol's messages (RISC OS's s/Messages, TaskSWI_Base) */
#define TASKWINDOW_INPUT   0x808C0u
#define TASKWINDOW_OUTPUT  0x808C1u
#define TASKWINDOW_EGO     0x808C2u
#define TASKWINDOW_MORIO   0x808C3u
#define TASKWINDOW_MORITE  0x808C4u
#define TASKWINDOW_NEWTASK 0x808C5u
#define TASKWINDOW_SUSPEND 0x808C6u
#define TASKWINDOW_RESUME  0x808C7u

/* Its errors (hdr/NewErrors, the block from &A80) */
#define TASKWINDOW_ERR_CANT_KILL  0xA80u
#define TASKWINDOW_ERR_BAD_HANDLE 0xA82u
#define TASKWINDOW_ERR_DYING      0xA83u
#define TASKWINDOW_ERR_FILE_SLEEP 0xA84u
#define TASKWINDOW_ERR_NO_EDITOR  0xA85u

/* The handles of the old form, in a tail that starts "<task> <txt> ".  Each
 * is eight hex digits followed by a space.  It returns 1 and the two
 * handles, or 0 for the *TaskWindow form. */
int taskwindow_parent_handles(const char *tail, uint32_t *task, uint32_t *txt);

/* Whether a character written goes to the output.  Without -ctrl a control
 * character is dropped, together with the bytes of its VDU sequence.
 * *counter counts those bytes down, and is 0 between sequences. */
int taskwindow_passes(uint8_t *counter, int ctrl, uint8_t ch);

#endif
