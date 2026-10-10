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
 * This file is a reimplementation in C of RISC OS Open's Squash module
 * (Sources/Programmer/Squash: c.compress, hdr.Squash).
 */

/* squash.h -- Squash, reimplemented (modules/squash): 12-bit LZW, in the
 * format of compress -b 12.
 *
 * Compiled code calls Squash_Compress and Squash_Decompress. The module
 * keeps their registers exactly. Native code calls the functions declared
 * here, with ordinary pointers. The flags are R0's, without bit 3, which
 * asks for the sizes and is dealt with below. Each operation does the same
 * as the SWI with those flags. It uses the fast algorithms where the SWI
 * would use them. It gives the same bytes out, the same status and the
 * same input and output used.
 *
 * The workspace has the size given and belongs to the caller. It holds all
 * of an operation's state between calls. It need not be arena memory, and
 * nor need the input and output. The SWIs check addresses as RISC OS does.
 * These functions do not.
 */
#ifndef ROSGD_SQUASH_H
#define ROSGD_SQUASH_H

#include <stdint.h>

#include "rosgd/error.h"

extern struct ros_module squash_module;

/* R0's flags */
#define SQUASH_CONTINUE  0x01u  /* go on with the operation in the workspace */
#define SQUASH_MORE      0x02u  /* more input follows this */
#define SQUASH_FAST      0x04u  /* decompressing: the output will all fit */
#define SQUASH_SIZES     0x08u  /* the SWIs: return the workspace size */

/* What an operation says it did (R0 out) */
#define SQUASH_DONE        0u   /* finished */
#define SQUASH_NEEDS_INPUT 1u   /* used what input it could; give it more */
#define SQUASH_NEEDS_ROOM  2u   /* the output is full; give it more room */

/* The workspace size each operation needs. squash_compress_max gives the
 * most output a compression can make of n bytes. That is 12 + 3n/2, worked
 * out in 32 bits as RISC OS does. */
#define SQUASH_COMPRESS_WORKSPACE   (31u * 1024)
#define SQUASH_DECOMPRESS_WORKSPACE (17u * 1024)
uint32_t squash_compress_max(uint32_t n);

/* An operation's input and output. Each pointer is moved past what the
 * operation used, as R2-R5 are. */
struct squash_io {
    const uint8_t *in;
    uint32_t in_left;
    uint8_t *out;
    uint32_t out_left;
};

os_error *squash_compress(uint32_t flags, void *workspace, struct squash_io *io,
                          uint32_t *status);
os_error *squash_decompress(uint32_t flags, void *workspace, struct squash_io *io,
                            uint32_t *status);

#endif
