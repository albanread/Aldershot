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

/* zlibmod.h: ZLib (Programmer/ZLibMod, 0.05) as a native module over
 * zlib itself (modules/zlibmod; README.md there lists the SWIs).
 *
 * RISC OS programs hold zlib streams in their own memory. This is a
 * 56-byte z_stream (hdr/ZLib's ZLib_Stream_*), the stream control block.
 * Its "state" word belongs to the module. Here the word names a record in
 * the RMA. The record holds the host's z_stream, and zlib allocates that
 * stream's own state in the RMA too. The block's public words are copied
 * in before each call and out after it. The gzip file calls (gz.c) work
 * over FileSwitch.
 */
#ifndef ROSGD_ZLIBMOD_H
#define ROSGD_ZLIBMOD_H

#include <stdint.h>

#include "rosgd/error.h"

extern struct ros_module zlib_module;

/* The stream control block: RISC OS's z_stream, 14 words */
#define ZM_SCB_SIZE     56u
#define ZM_NEXT_IN      0u
#define ZM_AVAIL_IN     4u
#define ZM_TOTAL_IN     8u
#define ZM_NEXT_OUT     12u
#define ZM_AVAIL_OUT    16u
#define ZM_TOTAL_OUT    20u
#define ZM_MSG          24u
#define ZM_STATE        28u
#define ZM_ZALLOC       32u
#define ZM_ZFREE        36u
#define ZM_OPAQUE       40u
#define ZM_DATA_TYPE    44u
#define ZM_ADLER        48u

/* hdr/NewErrors: ErrorBase_ZLib, &81F000 (Jeffrey Lee's, not ROL's) */
#define ZM_ERRBASE      0x81F000u
enum { ZM_E_NOMEM, ZM_E_INVCFLG, ZM_E_INVDFLG, ZM_E_ZLIBERR, ZM_E_UKSWI, ZM_E_INVTA,
       ZM_E_INVGZSK };

/* ---- shared by the module's files ----------------------------------------------- */

/* The module's own error, by its token's number (Resources.ZLib.Messages) */
os_error *zm_error(int which);
/* "ZLib error %0[: %1]": zlib's return code by name, and its message */
os_error *zm_zlib_error(int err, const char *msg);
/* RISC OS's answer where it would read or write memory that is not there:
 * the processor's abort, as an error */
os_error *zm_abort(uint32_t addr);
/* Whether [a, a + len) is memory a caller may use. A len of 0 always is. */
int zm_valid(uint32_t a, uint32_t len);
/* zlib's allocator: RMA blocks */
void *zm_zalloc(void *opaque, unsigned items, unsigned size);
void zm_zfree(void *opaque, void *p);
/* A string of zlib's (static, outside the arena) as an arena address. This
 * is a copy in the module's workspace, made once. NULL gives 0. */
uint32_t zm_string(const char *s);
/* A SWI from native code. Returns the error, or NULL. r holds the registers
 * in and out (8 words). */
os_error *zm_swi(uint32_t number, uint32_t *r);

/* ---- the gzip file calls (gz.c), RISC OS's registers ---------------------------- */

struct ros_cpu;
os_error *zm_gz_open(struct ros_cpu *s);
os_error *zm_gz_read(struct ros_cpu *s);
os_error *zm_gz_write(struct ros_cpu *s);
os_error *zm_gz_flush(struct ros_cpu *s);
os_error *zm_gz_close(struct ros_cpu *s);
os_error *zm_gz_error(struct ros_cpu *s);
os_error *zm_gz_seek(struct ros_cpu *s);
os_error *zm_gz_tell(struct ros_cpu *s);
os_error *zm_gz_eof(struct ros_cpu *s);
/* Close every file the module has open (finalisation) */
void zm_gz_close_all(void);

/* The module's workspace (the private word's block): what would be file
 * statics in C */
struct zm_workspace {
    uint32_t streams;           /* the first stream record (arena address), or 0 */
    uint32_t files;             /* the first gzip file, or 0 */
    uint32_t strings_used;
    char version[16];           /* ZLib_Version's string */
    char strings[2048];         /* zlib's messages, copied into the arena */
};
struct zm_workspace *zm_ws(void);

#endif
