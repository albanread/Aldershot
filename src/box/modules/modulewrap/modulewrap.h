/* Copyright RISC OS Open Ltd and others
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
 * This file is derived from RISC OS Open's source and from other code.
 * The Apache licence of RISC OS Open's source applies to it.
 */

/* modulewrap.h: the modules for the ROM's C applications (modulewrap.c).
 * "!Edit", "!Draw" and "!Paint" are the modules that RISC OS's ROM build
 * makes of Edit, Draw and Paint with RISC_OSLib's ModuleWrap. They are
 * native here. Each *Desktop_<Name> runs the application's x32 image from
 * ResourceFS. "Filer_Action" has ModuleWrap's FilerAct shape, and its
 * *Filer_Action runs Filer_Action's image.
 */
#ifndef ROSGD_MODULEWRAP_H
#define ROSGD_MODULEWRAP_H

/* The modules, for the ROM's list of native modules. */
extern struct ros_module edit_module;
extern struct ros_module drawapp_module;
extern struct ros_module paintapp_module;
extern struct ros_module fileract_module;

/* The application space an x32 image is given before it runs, in K. It
 * holds the image, crt0_x32's 256K root stack and the start of the heap.
 * This is the same as the WimpSlot in the disc !Run (tools/mkdiscapp.py).
 * Flex grows the space as files load. */
#define MODULEWRAP_EDIT_SLOT_K 640u
/* Draw's. Its image (286K in memory) and the root stack need 544K to
 * start. Below that the library gives error C01. 640K is the same as
 * Edit's and leaves room for the first drawing. Flex grows the space as
 * files load. */
#define MODULEWRAP_DRAW_SLOT_K 640u
/* Paint's. Its image (293K in memory, A64X32) and the root stack need 552K
 * to start. At 544K it fails with "Not enough memory for C library". 640K
 * is the same as Draw's and leaves room for the first sprite. Flex grows
 * the space as sprites are made and files load. */
#define MODULEWRAP_PAINT_SLOT_K 640u
/* Filer_Action's. Its image (110K) and the root stack start in 352K
 * (A64X32). At 320K it fails with "Not enough memory for C library".
 * 640K is the same as the others. Filer_Action grows the slot itself for
 * its copy buffer (memmanage's Wimp_SlotSize, flex) and gives the memory
 * back. 5.30's ModuleWrap gives the ROM code "*WimpSlot -min 40k -max 40k". */
#define MODULEWRAP_FILERACT_SLOT_K 640u

#endif
