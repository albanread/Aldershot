/* Copyright (c) 2014, RISC OS Open Ltd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of RISC OS Open Ltd nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is a reimplementation in C of RISC OS Open's MimeMap module
 * (Sources/Networking/MimeMap: c.mime, h.mime, hdr.MimeMap, cmhg.MimeMapHdr).
 */

/* mimemap.h -- MimeMap, a native module (mimemap.c). It is RISC OS 5.30's
 * ROM MimeMap 0.19, with MimeMap_Translate and its *commands. It works over
 * the MIME mappings file that the ROM carries (#106). */
#ifndef ROSGD_MIMEMAP_H
#define ROSGD_MIMEMAP_H

extern struct ros_module mimemap_module;

/* The formats from hdr/MimeMap. They are used in MimeMap_Translate's R0 and R2. */
#define MMM_TYPE_RISCOS        0u   /* a file type, in the register */
#define MMM_TYPE_RISCOS_STRING 1u   /* a file type's name (or &xxx) */
#define MMM_TYPE_MIME          2u   /* a MIME content type */
#define MMM_TYPE_DOT_EXTN      3u   /* a file extension */
#define MMM_TYPE_MAC           4u   /* an Apple type/creator: never answered */
#define MMM_TYPE_DOT_EXTNS     5u   /* the extensions, as an array (output only) */

/* These are where the ROM's table is, and what the module sets when nothing
 * else has set a path. InetDBase$Path is set as RISC OS's !Internet sets it,
 * to its files directory. Inet$MimeMappings is set as !Boot's BootRun sets
 * it. */
#define MIMEMAP_ROM_DBASE "Resources:$.Resources.Internet.files."
#define MIMEMAP_DEFAULT   "InetDBase:MimeMap"

#endif
