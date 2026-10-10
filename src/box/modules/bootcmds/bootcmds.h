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
 * This file is a reimplementation in C of RISC OS Open's BootCommands module
 * (Sources/Programmer/BootCmds: c.main, c.repeatcmd, cmhg.header, h.main).
 */

/* bootcmds.h: BootCommands, a native module.  It provides the * commands
 * that !Boot sequences are written in.  The code is in bootcmds.c and the
 * commands are listed in README.md.
 *
 * The module has no SWIs.  Its interface is its * commands.  C code can
 * do the same work through the functions declared here.  They take
 * ordinary strings and copy them to the arena where a SWI needs them.
 * Each returns NULL, or the error that the command would give.
 */
#ifndef ROSGD_BOOTCMDS_H
#define ROSGD_BOOTCMDS_H

#include <stdint.h>

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module bootcmds_module;

/* Errors, from ErrorBase_BootCommands (&81F300). */
#define BOOTCMDS_ERR_BAD_FILE 0x81F300u   /* "Corrupt CMOS file" */
#define BOOTCMDS_ERR_BAD_VER  0x81F301u   /* "CMOS file is for a different OS version" */
#define BOOTCMDS_ERR_NO_MEM   0x81F302u   /* the global "NoMem" */
#define BOOTCMDS_ERR_WANT_STR 0x81F303u   /* "System variable must contain a string" */

/* *AddApp: adds stub files to ResourceFS's Apps for each application that
 * spec names.  A wildcarded leaf can name several.  Each application gets
 * a !Run, a !Boot if it has a !Boot or a !Sprites, and a !Help if it has
 * one. */
os_error *bootcmds_add_app(const char *spec);

/* *Do: the line is passed through GSTrans and then run. */
os_error *bootcmds_do(const char *line);

/* *IfThere's test: whether OS_File 23 finds an object of that name. */
int bootcmds_there(const char *name);

/* *X: runs the command.  If it gives an error and X$Error is not set, the
 * error's text is put in X$Error. */
os_error *bootcmds_x(const char *command);

/* *SaveCMOS: writes the settings and the OS version word to a file. */
os_error *bootcmds_save_cmos(const char *file);

/* The work of *LoadCMOS.  It returns the error that the command would
 * print, because the command itself returns none.  It returns NULL once
 * the file's settings are loaded. */
os_error *bootcmds_load_cmos(const char *file);

/* *AppPath, *PrepPath and *RemPath: add an element to the end or the start
 * of a path variable, or remove it.  None of them leaves a duplicate. */
enum bootcmds_path_edit { BOOTCMDS_APPEND, BOOTCMDS_PREPEND, BOOTCMDS_REMOVE };
os_error *bootcmds_path(const char *variable, const char *element, enum bootcmds_path_edit how);

/* *Canonical: replaces a variable's value by its canonical form. */
os_error *bootcmds_canonical(const char *variable);

/* *AppSlot: sets the application space to size bytes. */
os_error *bootcmds_app_slot(uint32_t size);

/* *Repeat: runs command for each object of directory that flags choose.
 * Each command line is "<command> <directory>.<leaf> [<tail>]".  It returns
 * NULL, or the error that stopped it.  With CONTINUE no error stops it, and
 * the first one goes to X$Error.  The * command also reports that error and
 * sets Sys$ReturnCode, as the application it replaced did.  This function
 * does neither. */
#define BOOTCMDS_REPEAT_DIRECTORIES  (1u << 0)
#define BOOTCMDS_REPEAT_APPLICATIONS (1u << 1)
#define BOOTCMDS_REPEAT_FILES        (1u << 2)
#define BOOTCMDS_REPEAT_TYPE         (1u << 3)    /* files of the type in the type field */
#define BOOTCMDS_REPEAT_TASKS        (1u << 4)
#define BOOTCMDS_REPEAT_VERBOSE      (1u << 5)
#define BOOTCMDS_REPEAT_SORT         (1u << 6)
#define BOOTCMDS_REPEAT_CONTINUE     (1u << 7)
#define BOOTCMDS_REPEAT_PROGRESS     (1u << 8)    /* BootFX's progress bar, from start and on by range */
struct bootcmds_repeat {
    const char *command, *directory, *tail;     /* tail may be NULL */
    uint32_t flags, type;
    int start, range;
};
os_error *bootcmds_repeat(const struct bootcmds_repeat *r);

#endif
