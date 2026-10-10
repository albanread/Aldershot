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
 * This file is a reimplementation in C of RISC OS Open's ResourceFS
 * (Sources/FileSys/ResourceFS/ResourceFS: s.ResourceFS, s.MsgCode, hdr.ResourceFS).
 */

/* resourcefs.h: ResourceFS (resourcefs.c).
 *
 * The SWIs are the typed ResourceFS_* calls in the generated api.h. The
 * filing system, under FileSwitch, is ros_resourcefs_fs (fileswitch.h).
 * This header declares the rest. That is the modules for the ROM's list,
 * and the lookup that MessageTrans uses to find a file's data where it lies
 * in memory, as FileSwitch's ReadFSHandle gives it.
 */
#ifndef ROSGD_RESOURCEFS_H
#define ROSGD_RESOURCEFS_H

#include <stdint.h>

/* ResourceFS, and Messages, which registers the ROM's own files. */
extern struct ros_module resourcefs_module;
extern struct ros_module messages_module;

/* Find a file by name. The name is either "Resources:$.Resources.X" or a
 * name through a path variable, "Prefix:X", where Prefix$Path names
 * Resources: places. The result is an arena address, the word before the
 * file's data, which holds the file's length + 4. It is 0 if the name is
 * not a registered file. */
uint32_t ros_resourcefs_find(const char *name);

#endif
