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
 * This file is a reimplementation in C of RISC OS Open's Buffer Manager
 * (Sources/HWSupport/Buffers: s.Buffers, s.Buffer, s.MoveBlock, s.Errors, hdr.Buffer).
 */

/* buffers.h: the Buffer Manager's data operations, for C.
 *
 * The SWIs (Buffer_Create, Buffer_GetInfo and the rest) are in the
 * generated api.h.  The functions here do what InsV, RemV and CnpV do for
 * translated code, but with C types, on a buffer's handle. They put bytes
 * in, take them out, look without taking, count and purge.
 *
 * The data pointers are ordinary memory. Bytes are copied into and out
 * of the buffer. The buffer is the shared part. It is in the RMA, and any
 * task, or any process once the runtime has more than one, may be at the
 * other end of it.
 */
#ifndef ROSGD_BUFFERS_H
#define ROSGD_BUFFERS_H

#include <stdint.h>

#include "rosgd/error.h"

/* Put up to n bytes in: *left is how many did not fit. */
os_error *xbuffer_put(uint32_t handle, const void *data, uint32_t n, uint32_t *left);

/* Take up to n bytes out: *left is how many were not there. */
os_error *xbuffer_get(uint32_t handle, void *data, uint32_t n, uint32_t *left);

/* As xbuffer_get, leaving the bytes in the buffer. */
os_error *xbuffer_peek(uint32_t handle, void *data, uint32_t n, uint32_t *left);

/* Bytes in the buffer, and the space for more. The one-unit gap is not
 * counted as space.
 * Either pointer may be NULL. */
os_error *xbuffer_count(uint32_t handle, uint32_t *used, uint32_t *free_space);

os_error *xbuffer_purge(uint32_t handle);

/* The module, for the ROM's list of native modules. */
extern struct ros_module buffer_manager_module;

#endif
