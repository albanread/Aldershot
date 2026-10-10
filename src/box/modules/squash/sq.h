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
 * This file is a translation into C of the fast paths of RISC OS Open's Squash
 * module (Sources/Programmer/Squash: s.comp_ass, s.zcat_ass, s.zcat_ass12, h.defs).
 */

/* sq.h -- Squash's insides (modules/squash). These are the fast paths.
 * RISC OS writes them in ARM code and they are C here. The restartable
 * algorithms are RISC OS's own C, c/cssr and c/zssr, built from its sources
 * by the Makefile.
 */
#ifndef ROSGD_SQ_H
#define ROSGD_SQ_H

#include <stdint.h>

/* The 12-bit LZW of compress -b 12, as h/defs has it. */
#define SQ_BITS       12
#define SQ_HASH_RETRY 5003          /* the compressor's hash table, in words */
#define SQ_CLEAR      256
#define SQ_FIRST      257

/* s/comp_ass: compresses all of the input at once, into an output big
 * enough (3 + 3n/2 bytes). table is the workspace, SQ_HASH_RETRY words.
 * The output's address matters (see fast.c). Returns the bytes written.
 * n must be at least 1. */
uint32_t sq_compress_fast(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t *table);

/* s/zcat_ass (12 bits): decompresses all of the input at once. pointers is
 * the workspace, 4097 words. It writes at most room bytes, and *used is
 * what it wrote. It returns 0 when it finished, 1 if the output did not
 * hold it all, and -1 if the input refers to a code not made yet. */
int sq_decompress_fast(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t room,
                       uint32_t *pointers, uint32_t *used);

#endif
