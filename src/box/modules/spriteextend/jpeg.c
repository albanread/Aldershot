/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2010 Castle Technology Ltd
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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/Video/Render/SprExtend: c.rojpeg, c.romerge, c.jdcolor,
 * Sources/SWIs, c.PutScaled).
 */

/* Parts of this file follow the Independent JPEG Group's library as RISC OS's
 * SpriteExtend carries it (c.jdcolor, c.romerge), and so are subject to the
 * IJG's terms:
 *
 *   Copyright (C) 1991-1997, Thomas G. Lane.
 *   Modified 2011 by Guido Vollbeding.
 *   This software is based in part on the work of the Independent JPEG Group.
 *
 *   The authors make NO WARRANTY or representation, either express or
 *   implied, with respect to this software, its quality, accuracy,
 *   merchantability, or fitness for a particular purpose.  This software is
 *   provided "AS IS", and you, its user, assume the entire risk as to its
 *   quality and accuracy.
 *
 *   Permission is hereby granted to use, copy, modify, and distribute this
 *   software (or portions thereof) for any purpose, without fee, subject to
 *   these conditions:
 *   (1) If any part of the source code for this software is distributed,
 *   then the IJG's README file must be included, with its copyright and
 *   no-warranty notice unaltered; and any additions, deletions, or changes
 *   to the original files must be clearly indicated in accompanying
 *   documentation.
 *   (2) If only executable code is distributed, then the accompanying
 *   documentation must state that "this software is based in part on the
 *   work of the Independent JPEG Group".
 *   (3) Permission for use of this software is granted only if the user
 *   accepts full responsibility for any undesirable consequences; the
 *   authors accept NO LIABILITY for damages of any kind.
 *
 *   Permission is NOT granted for the use of any IJG author's name or
 *   company name in advertising or publicity relating to this software or
 *   products derived from it.
 *
 * The merged upsampler (c.romerge) is also RISC OS Open's:
 *
 *   Copyright (c) 2015, RISC OS Open Ltd
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in
 *         the documentation and/or other materials provided with the
 *         distribution.
 *       * Neither the name of RISC OS Open Ltd nor the names of its
 *         contributors may be used to endorse or promote products derived
 *         from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 *   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 *   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* jpeg.c -- SpriteExtend's JPEG SWIs: JPEG_Info, JPEG_FileInfo,
 * JPEG_PlotScaled, JPEG_PlotFileScaled, JPEG_PlotTransformed,
 * JPEG_PlotFileTransformed and JPEG_PDriverIntercept, as RISC OS 5.30's
 * SpriteExtend 1.86 has them (Sources/SWIs, c/rojpeg, c/PutScaled).
 *
 * The original decodes with its own copy of the IJG's library (release 9,
 * the fast integer IDCT, plain upsampling, and its own merged upsampler
 * and colour conversion in c/romerge and c/jdcolor).  It decodes a band of
 * MCU rows at a time.  It plots through its scaled-sprite code generator
 * as a 32bpp "sprite" of type 9.
 *
 * Here libjpeg-turbo does the entropy decoding and the same fast integer
 * IDCT (JDCT_IFAST).  It is the library in /init that CompressJPEG and
 * ChangeFSI's CFSI use.  It gives each component's samples (raw data out).
 * The upsampling and colour conversion are the original's, from its
 * sources.  The plot is scaled.c's, the scaled-sprite path, so clipping,
 * depths, tables and dither are those of sprites.
 *
 * JPEG_PlotTransformed only scales, as the original does.  A matrix with a
 * rotation, or a coordinate block that is not an upright rectangle, is
 * refused ("Transformed JPEG plotting is not supported").
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <jerror.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "sprext.h"

/* ---- errors (hdr/NewErrors, Resources Messages) ------------------------------------------------- */

enum {
    E_BADJPEG = 0x71A, E_BADJPEGPLOT = 0x71B, E_JPEGNOROOM = 0x71C, E_JPEGFATAL = 0x71D,
    E_UNSUPPORTED = 0x721,
};

static os_error *jerr(uint32_t n)
{
    switch (n) {
    case E_BADJPEG: return ros_error(n, "Incomplete or corrupt JPEG data");
    case E_BADJPEGPLOT:
        return ros_error(n, "Transformed JPEG plotting is not supported by this version of the "
                            "SpriteExtend module");
    case E_JPEGNOROOM: return ros_error(n, "Not enough memory available to plot JPEG");
    case E_JPEGFATAL: return ros_error(n, "JPEG plot failed due to fatal inconsistency");
    default:
        return ros_error(E_UNSUPPORTED, "JPEG format is not supported by this version of the "
                                        "SpriteExtend module");
    }
}

/* ---- the module's JPEG state, in its workspace ---------------------------------------------------- */

struct sx_jstate {
    uint32_t intercept;                 /* JPEG_PDriverIntercept's flags */
    uint32_t ws_size;                   /* the decompression workspace, 0 before the first plot */
    uint32_t table[3];                  /* ColourTrans's 32K table descriptor */
    uint32_t palette[256];
};

static struct sx_jstate *jstate(void)
{
    uint32_t pw = spriteextend_module.private_word;
    uint32_t a = pw ? ros_ld32(pw) : 0;
    return a ? ros_ptr(a) : NULL;
}

os_error *sx_jpeg_init(void)
{
    void *b;
    if (xos_module_claim(sizeof(struct sx_jstate), &b))
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(b, 0, sizeof(struct sx_jstate));
    ros_st32(spriteextend_module.private_word, ros_addr(b));
    return NULL;
}

void sx_jpeg_final(void)
{
    struct sx_jstate *st = jstate();
    if (st)
        xos_module_free(st);
    ros_st32(spriteextend_module.private_word, 0);
}

/* ---- the header (jpeg_find_image_dims) ------------------------------------------------------------ */

/* A JPEG's bytes, read with a bound: in memory, as far as the arena is
 * readable (the original reads on regardless of the length given); from a
 * file, the file */
struct jsrc {
    const uint8_t *p;
    uint32_t addr, valid;               /* in memory: bytes known readable */
    uint32_t len;                       /* a file's length; in memory the length given */
    int file;
};

static int readable(struct jsrc *s, uint32_t off)
{
    if (s->file)
        return off < s->len;
    while (off >= s->valid) {
        if (s->valid >= 0x1000000u)
            return 0;
        uint32_t a = s->addr + s->valid, end = (a | 0xFFFu) + 1;
        int invalid = 1;
        if (a < s->addr || xos_validate_address(a, end, &invalid) || invalid)
            return 0;
        s->valid = end - s->addr;
    }
    return 1;
}

struct jdims {
    int w, h, type;                     /* type: components, SOF in bits 8-11, bit 7 ratio */
    uint32_t density;                   /* x << 16 | y */
    int max_h, max_v;
};

enum { JFID_OK, JFID_NOT_JPEG, JFID_CANT_RENDER };

static int find_dims(struct jsrc *s, struct jdims *d)
{
    int hadapp0 = 0, hadsof = 0, type = 0, maxh = 1, maxv = 1, w = 0, h = 0;
    uint32_t density = 0, at = 0, advance;
    if (!readable(s, 1) || s->p[0] != 0xFF || s->p[1] != 0xD8)
        return JFID_NOT_JPEG;
    advance = 2;
    while (!hadsof) {
        at += advance;
        if (!readable(s, at + 20))
            return JFID_NOT_JPEG;
        const uint8_t *j = s->p + at;
        if (j[0] == 0xFF) {
            advance = 1;
            continue;
        }
        uint32_t len = (uint32_t)j[1] << 8 | j[2];
        switch (j[0]) {
        case 0:
            advance = 1;
            continue;
        case 0xDA: case 0xD9:
            return JFID_NOT_JPEG;
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return JFID_CANT_RENDER;
        case 0xC9: case 0xCA: case 0xC0: case 0xC1: case 0xC2: {
            if (!readable(s, at + 10 + 3u * j[8]))
                return JFID_NOT_JPEG;
            if (len < 8 || len < 8u + j[8] * 3u)
                return JFID_NOT_JPEG;
            hadsof = 1;
            int comps = j[8];
            h = j[4] << 8 | j[5];
            w = j[6] << 8 | j[7];
            type |= comps | (j[0] & 0xF) << 8;
            maxv = maxh = 1;
            for (int i = 0; i < comps; i++) {
                uint8_t c = j[10 + i * 3];
                if ((c & 0xF) > maxv)
                    maxv = c & 0xF;
                if ((c >> 4) > maxh)
                    maxh = c >> 4;
            }
#define BADSAMP(f) ((f) > 4 || ((f) & ((f) - 1)) || !(f))
            if ((comps != 1 && comps != 3 && comps != 4) || j[3] != 8 || BADSAMP(maxv) ||
                BADSAMP(maxh))
                return JFID_CANT_RENDER;
            break;
        }
        case 0xE0:
            if (len < 16 || j[3] != 'J' || j[4] != 'F' || j[5] != 'I' || j[6] != 'F')
                break;
            if (j[8] != 1)
                return JFID_CANT_RENDER;
            hadapp0 = 1;
            switch (j[10]) {
            case 0: {
                int xd = 90, yd = j[11] << 8 | j[12];
                if (yd == 0)
                    yd = 1;
                yd = (j[13] << 8 | j[14]) / yd;
                yd = yd == 0 ? 90 : yd * 90;
                density = (uint32_t)xd << 16 | (uint32_t)yd;
                type |= 0x80;
                break;
            }
            case 1:
                density = (uint32_t)j[11] << 24 | (uint32_t)j[12] << 16 | (uint32_t)j[13] << 8 | j[14];
                break;
            case 2: {
                int xd = ((j[11] << 8) + j[12]) * 254 / 100, yd = ((j[13] << 8) + j[14]) * 254 / 100;
                density = (uint32_t)xd << 16 | (uint32_t)yd;
                break;
            }
            default:
                return JFID_CANT_RENDER;
            }
            break;
        case 0x01: case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
        case 0xD7:
            len = 0;
            break;
        default:
            break;
        }
        advance = len + 2;
    }
    if (!hadapp0 || density == 0) {
        type |= 0x80;
        density = 90u << 16 | 90u;
    }
    d->w = w, d->h = h, d->type = type, d->density = density, d->max_h = maxh, d->max_v = maxv;
    return JFID_OK;
}

/* The workspace the original asks for (sizeof its decompress struct, 712;
 * its huff_pointer, 20): the first JPEG's with 10K to spare, a later one's
 * beyond what it has, in pages */
static uint32_t ws_needed(const struct jdims *d)
{
    uint32_t mh = (uint32_t)d->max_v * 8, mw = (uint32_t)d->max_h * 8;
    uint32_t n = 712 + 24 * 1024 + 20 * (((uint32_t)d->h + mh - 1) / mh) +
                 (mh > 2 ? mh : 2) * ((uint32_t)d->w + mw - 1) * 4;
    n = (n + 3) & ~3u;
    struct sx_jstate *st = jstate();
    uint32_t have = st ? st->ws_size : 0;
    if (!have)
        n += 10 * 1024;
    else
        n = have >= n ? 0 : n - have;
    return (n + 4095) & ~4095u;
}

static void ws_claim(uint32_t extra)
{
    struct sx_jstate *st = jstate();
    if (st)
        st->ws_size += extra;
}

static os_error *dims_error(int code)
{
    return jerr(code == JFID_NOT_JPEG ? E_BADJPEG : E_UNSUPPORTED);
}

/* A file's bytes, whole, for FileInfo and the file plots: the open's error
 * as FileSwitch gives it */
static os_error *load_file(uint32_t name, uint8_t **out, uint32_t *len)
{
    uint32_t r[10] = { 0x4F, name };
    os_error *e;
    sx_swi(XOS_Find, r, &e);
    if (e)
        return e;
    uint32_t h = r[0];
    uint32_t a[10] = { 2, h };
    sx_swi(XOS_Args, a, &e);
    uint32_t n = e ? 0 : a[2];
    uint8_t *buf = NULL;
    if (!e) {
        void *b;
        if (xos_module_claim(n ? n : 4, &b))
            e = jerr(E_JPEGNOROOM);
        else
            buf = b;
    }
    if (!e && n) {
        uint32_t g[10] = { 3, h, ros_addr(buf), n, 0 };
        sx_swi(XOS_GBPB, g, &e);
    }
    uint32_t c[10] = { 0, h };
    os_error *e2;
    sx_swi(XOS_Find, c, &e2);
    if (e) {
        if (buf)
            xos_module_free(buf);
        return e;
    }
    *out = buf, *len = n;
    return NULL;
}

/* JPEG_Info, JPEG_FileInfo */
static os_error *info(struct ros_cpu *s, int file)
{
    uint32_t flags = s->r[0];
    if (flags & ~3u)
        return sx_err(SX_BADFLAGS);
    struct jsrc src = { 0 };
    uint8_t *fbuf = NULL;
    os_error *e;
    if (file) {
        uint32_t n;
        if ((e = load_file(s->r[1], &fbuf, &n)) != NULL)
            return e;
        src.p = fbuf, src.len = n, src.file = 1;
    } else {
        src.p = ros_ptr(s->r[1]), src.addr = s->r[1], src.len = s->r[2];
    }
    struct jdims d;
    int code = find_dims(&src, &d);
    if (fbuf)
        xos_module_free(fbuf);
    if (code != JFID_OK)
        return dims_error(code);
    uint32_t r0 = (d.type & 7) == 1 ? 1 : 0;
    if (d.type & 0x80)
        r0 |= 4;
    if (flags & 2)
        r0 |= (uint32_t)(d.type >> 8) << 3;
    r0 |= 2;                            /* "transformed plots not supported" */
    s->r[0] = r0;
    if (flags & 1)
        s->r[2] = (uint32_t)d.w, s->r[3] = (uint32_t)d.h;
    s->r[4] = d.density >> 16, s->r[5] = d.density & 0xFFFF;
    s->r[6] = ws_needed(&d);
    return NULL;
}

/* ---- decoding: each component's samples ---------------------------------------------------------- */

enum { CS_GREY = 1, CS_YCC, CS_RGB, CS_CMYK, CS_YCCK };

struct jcomp {
    int hs, vs;                         /* sampling factors */
    int pw, ph;                         /* the plane, padded to whole blocks and iMCU rows */
    uint8_t *p;
};

struct jimg {
    int w, h, nc, cs, max_h, max_v;
    int dc_only;
    struct jcomp c[4];
};

extern char compressjpeg_host_memory;   /* modules/compressjpeg: libjpeg's memory manager */

struct jerrmgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jerr_exit(j_common_ptr cinfo)
{
    longjmp(((struct jerrmgr *)cinfo->err)->jb, 1);
}

static void jerr_output(j_common_ptr cinfo)
{
    (void)cinfo;
}

/* A warning: the data ending before the image does is an error here, as the
 * original's first plot of such a JPEG fails ("Incomplete or corrupt JPEG
 * data"; a later plot of it draws whatever its workspace held) */
static void jerr_emit(j_common_ptr cinfo, int level)
{
    if (level < 0 && cinfo->err->msg_code == JWRN_JPEG_EOF)
        longjmp(((struct jerrmgr *)cinfo->err)->jb, 1);
}

static void jimg_free(struct jimg *im)
{
    for (int i = 0; i < 4; i++)
        free(im->c[i].p);
    memset(im, 0, sizeof *im);
}

/* The DC-only value of a block, as the original's fast IDCT makes it from
 * a block with no AC terms: the multiplier table's DC entry is the
 * quantiser times 4, the descale is 5 bits, and the IDCT's range limit */
static uint8_t dc_sample(int coef, int q)
{
    int v = (coef * q * 4) >> 5;
    v &= 1023;
    if (v < 128)
        return (uint8_t)(v + 128);
    if (v < 512)
        return 255;
    if (v < 896)
        return 0;
    return (uint8_t)(v - 896);
}

/* Decodes the JPEG at data (len bytes) into *im; 0, or an error number */
static uint32_t decode(const uint8_t *data, uint32_t len, int dc_only, struct jimg *im)
{
    struct jpeg_decompress_struct cinfo;
    struct jerrmgr err;
    memset(im, 0, sizeof *im);
    memset(&cinfo, 0, sizeof cinfo);
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jerr_exit;
    err.pub.output_message = jerr_output;
    err.pub.emit_message = jerr_emit;
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&cinfo);
        jimg_free(im);
        return E_BADJPEG;
    }
    cinfo.client_data = &compressjpeg_host_memory;  /* host memory, not the RMA */
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, len);
    jpeg_read_header(&cinfo, TRUE);
    im->w = (int)cinfo.image_width, im->h = (int)cinfo.image_height;
    im->nc = cinfo.num_components;
    switch (cinfo.jpeg_color_space) {
    case JCS_GRAYSCALE: im->cs = CS_GREY; break;
    case JCS_YCbCr: im->cs = CS_YCC; break;
    case JCS_RGB: im->cs = CS_RGB; break;
    case JCS_CMYK: im->cs = CS_CMYK; break;
    case JCS_YCCK: im->cs = CS_YCCK; break;
    default: im->cs = 0; break;
    }
    if (!im->cs || (im->nc != 1 && im->nc != 3 && im->nc != 4) || cinfo.data_precision != 8)
        longjmp(err.jb, 1);
    im->max_h = cinfo.max_h_samp_factor, im->max_v = cinfo.max_v_samp_factor;
    im->dc_only = dc_only;
    int mcu_rows = ((int)cinfo.image_height + im->max_v * 8 - 1) / (im->max_v * 8);
    for (int ci = 0; ci < im->nc; ci++) {
        jpeg_component_info *cp = &cinfo.comp_info[ci];
        struct jcomp *c = &im->c[ci];
        c->hs = cp->h_samp_factor, c->vs = cp->v_samp_factor;
        int wb = (((int)cinfo.image_width * c->hs + im->max_h - 1) / im->max_h + 7) / 8;
        c->pw = (wb + c->hs) * 8;       /* room for the iMCU's padding blocks */
        c->ph = mcu_rows * c->vs * 8;
        c->p = calloc((size_t)c->pw * (size_t)c->ph, 1);
        if (!c->p)
            longjmp(err.jb, 1);
    }
    if (dc_only) {
        jvirt_barray_ptr *coefs = jpeg_read_coefficients(&cinfo);
        for (int ci = 0; ci < im->nc; ci++) {
            jpeg_component_info *cp = &cinfo.comp_info[ci];
            struct jcomp *c = &im->c[ci];
            int q = cp->quant_table ? cp->quant_table->quantval[0] : 1;
            for (JDIMENSION by = 0; by < cp->height_in_blocks; by++) {
                JBLOCKARRAY rows = (*cinfo.mem->access_virt_barray)((j_common_ptr)&cinfo, coefs[ci],
                                                                     by, 1, FALSE);
                for (JDIMENSION bx = 0; bx < cp->width_in_blocks; bx++) {
                    uint8_t v = dc_sample(rows[0][bx][0], q);
                    for (int y = 0; y < 8; y++) {
                        int py = (int)by * 8 + y;
                        if (py >= c->ph)
                            break;
                        memset(c->p + (size_t)py * c->pw + bx * 8, v, 8);
                    }
                }
            }
        }
        jpeg_finish_decompress(&cinfo);
    } else {
        cinfo.raw_data_out = TRUE;
        cinfo.dct_method = JDCT_IFAST;
        cinfo.do_fancy_upsampling = FALSE;
        cinfo.out_color_space = cinfo.jpeg_color_space;
        jpeg_start_decompress(&cinfo);
        JSAMPROW rows[4][32];
        JSAMPARRAY planes[4];
        int lines = im->max_v * 8;
        while (cinfo.output_scanline < cinfo.output_height) {
            int mcu = (int)cinfo.output_scanline / lines;
            for (int ci = 0; ci < im->nc; ci++) {
                struct jcomp *c = &im->c[ci];
                for (int r = 0; r < c->vs * 8; r++)
                    rows[ci][r] = c->p + (size_t)(mcu * c->vs * 8 + r) * c->pw;
                planes[ci] = rows[ci];
            }
            if (jpeg_read_raw_data(&cinfo, planes, (JDIMENSION)lines) == 0)
                break;
        }
        jpeg_finish_decompress(&cinfo);
    }
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

/* The last image decoded, kept for the next plot of the same JPEG (a
 * window's redraw plots it a rectangle at a time): host memory, keyed by
 * the bytes themselves */
static struct {
    uint32_t addr, len, hash;
    int dc_only, valid;
    struct jimg im;
} cache;

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t decoded(uint32_t addr, uint32_t len, int dc_only, struct jimg **out)
{
    const uint8_t *data = ros_ptr(addr);
    uint32_t h = fnv(data, len);
    if (cache.valid && cache.addr == addr && cache.len == len && cache.hash == h &&
        cache.dc_only == dc_only) {
        *out = &cache.im;
        return 0;
    }
    if (cache.valid)
        jimg_free(&cache.im);
    cache.valid = 0;
    uint32_t e = decode(data, len, dc_only, &cache.im);
    if (e)
        return e;
    cache.addr = addr, cache.len = len, cache.hash = h, cache.dc_only = dc_only, cache.valid = 1;
    *out = &cache.im;
    return 0;
}

/* ---- colour conversion (c/romerge, c/jdcolor) ---------------------------------------------------- */

#define SCALEBITS 16
#define ONE_HALF  ((int32_t)1 << (SCALEBITS - 1))
#define FIX(x)    ((int32_t)((x) * (1L << SCALEBITS) + 0.5))

static int crr[256], cbb[256];
static int32_t crg[256], cbg[256];

static void ycc_tables(void)
{
    if (crr[0])
        return;
    for (int i = 0; i < 256; i++) {
        int32_t x = i - 128;
        crr[i] = (int)((FIX(1.40200) * x + ONE_HALF) >> SCALEBITS);
        cbb[i] = (int)((FIX(1.77200) * x + ONE_HALF) >> SCALEBITS);
        crg[i] = -FIX(0.71414) * x;
        cbg[i] = -FIX(0.34414) * x + ONE_HALF;
    }
}

static inline int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static inline uint8_t sample(const struct jimg *im, int ci, int x, int y)
{
    const struct jcomp *c = &im->c[ci];
    int sx = x / (im->max_h / c->hs), sy = y / (im->max_v / c->vs);
    return c->p[(size_t)sy * c->pw + sx];
}

/* The pixel at (x, y), top row 0, as the original's band buffer has it:
 * &0BGR */
static uint32_t rgb_at(const struct jimg *im, int x, int y)
{
    switch (im->cs) {
    case CS_GREY: {
        uint32_t g = sample(im, 0, x, y);
        return g | g << 8 | g << 16;
    }
    case CS_YCC: {
        int Y = sample(im, 0, x, y), cb = sample(im, 1, x, y), cr = sample(im, 2, x, y);
        int r = clamp255(Y + crr[cr]);
        int g = clamp255(Y + (int)((cbg[cb] + crg[cr]) >> SCALEBITS));
        int b = clamp255(Y + cbb[cb]);
        return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16;
    }
    case CS_RGB:
        return (uint32_t)sample(im, 0, x, y) | (uint32_t)sample(im, 1, x, y) << 8 |
               (uint32_t)sample(im, 2, x, y) << 16;
    case CS_CMYK: {
        int key = sample(im, 3, x, y);
        if (!key)
            return 0;
        key++;
        return (uint32_t)((sample(im, 0, x, y) * key) / 256 & 255) |
               (uint32_t)((sample(im, 1, x, y) * key) / 256 & 255) << 8 |
               (uint32_t)((sample(im, 2, x, y) * key) / 256 & 255) << 16;
    }
    default: {                          /* YCCK */
        int key = sample(im, 3, x, y);
        if (!key)
            return 0;
        int Y = sample(im, 0, x, y), cb = sample(im, 1, x, y), cr = sample(im, 2, x, y);
        int c = clamp255(255 - (Y + crr[cr]));
        int m = clamp255(255 - (Y + (int)((cbg[cb] + crg[cr]) >> SCALEBITS)));
        int yy = clamp255(255 - (Y + cbb[cb]));
        key++;
        return (uint32_t)((c * key) / 256 & 255) | (uint32_t)((m * key) / 256 & 255) << 8 |
               (uint32_t)((yy * key) / 256 & 255) << 16;
    }
    }
}

/* ---- the VIDC1 tables (c/genyuvtabs, built into the original) ------------------------------------- */

static uint8_t yuv_to_pixel[8192];
static uint32_t pixel_to_yuv[256];

static int rgb_to_yuv(int r, int g, int b)
{
    double yt = 0.29900 * (float)r + 0.58700 * (float)g + 0.11400 * (float)b;
    double ut = -0.16874 * (float)r - 0.33126 * (float)g + 0.50000 * (float)b + 256.0 / 2.0;
    double vt = 0.50000 * (float)r - 0.41869 * (float)g - 0.08131 * (float)b + 256.0 / 2.0;
    int y = (int)yt, u = (int)ut, v = (int)vt;
    y = clamp255(y), u = clamp255(u), v = clamp255(v);
    return (y << 16) + (u << 8) + v;
}

static int yuv_to_rgb(int y, int u, int v)
{
    double rt = (float)y + 1.40200 * (float)(v - 256.0 / 2.0);
    double gt = (float)y - 0.34414 * (float)(u - 256.0 / 2.0) - 0.71414 * (float)(v - 256.0 / 2.0);
    double bt = (float)y + 1.77200 * (float)(u - 256.0 / 2.0);
    int r = clamp255((int)rt), g = clamp255((int)gt), b = clamp255((int)bt);
    return (b << 16) + (g << 8) + r;
}

static int vidc_rgb(int i)
{
    int r = 0, g = 0, b = 0;
    if (i & 128) b |= 128;
    if (i & 64) g |= 128;
    if (i & 32) g |= 64;
    if (i & 16) r |= 128;
    if (i & 8) b |= 64;
    if (i & 4) r |= 64;
    if (i & 2) r |= 32, g |= 32, b |= 32;
    if (i & 1) r |= 16, g |= 16, b |= 16;
    r |= r >> 4, g |= g >> 4, b |= b >> 4;
    return (b << 16) + (g << 8) + r;
}

static void vidc_tables(void)
{
    static int made;
    if (made)
        return;
    int pal[256];
    for (int i = 0; i < 256; i++) {
        pal[i] = vidc_rgb(i);
        pixel_to_yuv[i] = (uint32_t)rgb_to_yuv(pal[i] & 255, (pal[i] >> 8) & 255, (pal[i] >> 16) & 255);
    }
    int k = 0;
    for (int y = 0; y < 256; y += 8)
        for (int u = 0; u < 256; u += 16)
            for (int v = 0; v < 256; v += 16) {
                int rgb = yuv_to_rgb(y + (y >> 5), u + (u >> 4), v + (v >> 4));
                int r = rgb & 255, g = (rgb >> 8) & 255, b = (rgb >> 16) & 255;
                int best = 1000000, mini = 0;
                for (int i = 0; i < 256; i++) {
                    int dg = ((pal[i] >> 8) & 255) - g, dist = dg * dg;
                    if (dist < best) {
                        int dr = (pal[i] & 255) - r, db = ((pal[i] >> 16) & 255) - b;
                        dist += dr * dr + db * db;
                        if (dist < best)
                            mini = i, best = dist;
                    }
                }
                yuv_to_pixel[k++] = (uint8_t)mini;
            }
    made = 1;
}

/* ---- error diffusion (Sources/diffuse), transliterated ------------------------------------------- */

struct diffuse {
    const uint8_t *table;               /* 15-bit RGB to a pixel, or NULL */
    const uint32_t *palette;            /* &BBGGRR00 */
    int32_t r, g, b;
};

static uint32_t space_out(uint32_t v)
{
    uint32_t t = v & 0xFF0000u;
    v += t;
    v += t << 1;
    t = v & ~0xFFu;
    v += t;
    v += t << 1;
    return v + 0x180u + (0x180u << 10) + (0x180u << 20);
}

static void clip_above(struct diffuse *f)
{
    if (f->r > 255) f->r = 255;
    if (f->g > 255) f->g = 255;
    if (f->b > 255) f->b = 255;
}

static uint32_t lookup(const struct diffuse *f)
{
    uint32_t t1 = (uint32_t)(f->r >> 3) | (uint32_t)(f->g >> 3) << 5 | (uint32_t)(f->b >> 3) << 10;
    if (f->table)
        return f->table[t1];
    uint32_t tint = ((t1 & 0x1C00) >> 10) + ((t1 & 0xE0) >> 5) + (t1 & 7);
    tint += tint << 2;
    uint32_t hi = 0;
    if (t1 & 0x4000) hi |= 128;
    if (t1 & 0x2000) hi |= 8;
    if (t1 & 0x200) hi |= 64;
    if (t1 & 0x100) hi |= 32;
    if (t1 & 0x10) hi |= 16;
    if (t1 & 0x08) hi |= 4;
    return hi | tint >> 5;
}

/* ConvertPixelPlusRGBTo8bit / ConvertSpacedPixelPlusRGBTo8bit, then the
 * error left: the pixel chosen */
static uint32_t choose(struct diffuse *f, uint32_t t, int spaced)
{
    if (spaced) {
        f->r += (int32_t)((t << 22) >> 22) - 0x180;
        if (f->r < 0) f->r = 0;
        f->g += (int32_t)((t << 12) >> 22) - 0x180;
        if (f->g < 0) f->g = 0;
        f->b += (int32_t)(t >> 20) - 0x180;
        if (f->b < 0) f->b = 0;
    } else {
        f->r += (int32_t)(t & 255);
        if (f->r < 0) f->r = 0;
        f->g += (int32_t)((t >> 8) & 255);
        if (f->g < 0) f->g = 0;
        f->b += (int32_t)((t >> 16) & 255);
        if (f->b < 0) f->b = 0;
    }
    clip_above(f);
    uint32_t idx = lookup(f);
    uint32_t p = f->palette[idx & 255];
    f->r -= (int32_t)((p & 0xFF00) >> 8);
    f->g -= (int32_t)((p & 0xFF0000) >> 16);
    f->b -= (int32_t)(p >> 24);
    return idx;
}

static uint32_t add_part(uint32_t w, int32_t r, int32_t g, int32_t b)
{
    return w + (uint32_t)r + ((uint32_t)g << 10) + ((uint32_t)b << 20);
}

/* asm_diffuse_to_8bpp / _24bpp: rows[0..n-1] (n >= 2), count pixels from
 * x0 in each; the chosen pixels to out[row][x].  The rows are changed, as
 * the original changes the band buffer. */
static void diffuse_block(struct diffuse *f, uint32_t **rows, int n, int count, int x0,
                          uint32_t **out)
{
    for (int line = 0; line < n; line++) {
        uint32_t *in = rows[line] + x0, *o = out[line] + x0;
        f->r = f->g = f->b = 0;
        int last = line == n - 1, first = line == 0;
        if (last) {
            for (int i = 0; i < count; i++) {
                o[i] = choose(f, in[i], 1);
                f->r = (f->r * 12) >> 4, f->g = (f->g * 12) >> 4, f->b = (f->b * 12) >> 4;
            }
            break;
        }
        uint32_t *nl = rows[line + 1] + x0;
        uint32_t prev = 0, pix = space_out(nl[0]), next = space_out(nl[1]);
        int np = 2, c = count, i = 0;
        if (--c > 0) {
            o[i] = choose(f, in[i], !first);
            i++;
            if (first)
                pix = add_part(pix, (f->r * 3) >> 4, (f->g * 3) >> 4, (f->b * 3) >> 4);
            else
                pix = add_part(pix, f->r >> 1, f->g >> 1, f->b >> 1);
            next = add_part(next, f->r >> 4, f->g >> 4, f->b >> 4);
            f->r = (f->r * 7) >> 4, f->g = (f->g * 7) >> 4, f->b = (f->b * 7) >> 4;
            prev = pix, pix = next, next = space_out(nl[np++]);
            while (--c > 0) {
                o[i] = choose(f, in[i], !first);
                i++;
                prev = add_part(prev, (f->r * 5) >> 4, (f->g * 5) >> 4, (f->b * 5) >> 4);
                pix = add_part(pix, (f->r * 3) >> 4, (f->g * 3) >> 4, (f->b * 3) >> 4);
                next = add_part(next, f->r >> 4, f->g >> 4, f->b >> 4);
                f->r = (f->r * 7) >> 4, f->g = (f->g * 7) >> 4, f->b = (f->b * 7) >> 4;
                nl[np - 3] = prev;
                prev = pix, pix = next, next = space_out(nl[np++]);
            }
        }
        /* the last pixel of the line */
        o[i] = choose(f, in[i], !first);
        prev = add_part(prev, (f->r * 5) >> 4, (f->g * 5) >> 4, (f->b * 5) >> 4);
        pix = add_part(pix, (f->r * 3) >> 4, (f->g * 3) >> 4, (f->b * 3) >> 4);
        if (!first)
            pix = add_part(pix, f->r >> 1, f->g >> 1, f->b >> 1);
        if (np >= 3)
            nl[np - 3] = prev;
        nl[np - 2] = pix;
    }
}

/* ---- the merged upsampler's own outputs (c/romerge), 2x2 YCbCr only ------------------------------- */

static int is_merged(const struct jimg *im)
{
    return im->cs == CS_YCC && im->nc == 3 && im->c[0].hs == 2 && im->c[0].vs == 2 &&
           im->c[1].hs == 1 && im->c[1].vs == 1 && im->c[2].hs == 1 && im->c[2].vs == 1;
}

/* h2v2_merged_upsample_16bpp: rows y, y+1 (y even) with its 2x2 dither */
static void merged16(const struct jimg *im, const struct sx_dest *d, int y, int sy, uint16_t *o0,
                     uint16_t *o1, int xmin, int xmax)
{
    int l2 = 4, rpos = 0, gpos = 5, bpos = 10, gt = 3, rbt = 3;
    uint32_t alpha = 0x8000;
    (void)l2;
    if (d->flags & 0x80)
        gpos = 5, bpos = 11, gt = 2, rbt = 3, alpha = 0;
    else if (d->ncolour < 4096)
        gpos = 4, bpos = 8, gt = 4, rbt = 4, alpha = 0xF000;
    if (!(d->flags & 0x8000))
        alpha = 0;
    if (d->flags & 0x4000) {
        int t = rpos;
        rpos = bpos, bpos = t;
    }
    (void)y;
    for (int x = xmin & ~1; x < ((xmax + 1) & ~1); x += 2) {
        int cb = sample(im, 1, x, sy), cr = sample(im, 2, x, sy);
        int cred = crr[cr], cgreen = (int)((cbg[cb] + crg[cr]) >> SCALEBITS), cblue = cbb[cb];
        static const int add[2][2] = { { 0, 4 }, { 6, 2 } };
        for (int r = 0; r < 2; r++) {
            uint16_t *o = r ? o1 : o0;
            if (!o)
                continue;
            for (int k = 0; k < 2; k++) {
                int Y = add[r][k] + im->c[0].p[(size_t)(sy + r) * im->c[0].pw + x + k];
                uint32_t v = (uint32_t)(clamp255(Y + cred) >> rbt) << rpos |
                             (uint32_t)(clamp255(Y + cgreen) >> gt) << gpos |
                             (uint32_t)(clamp255(Y + cblue) >> rbt) << bpos | alpha;
                o[x + k] = (uint16_t)v;
            }
        }
    }
}

/* h2v2_merged_upsample_8bpp_vidc: rows sy, sy+1 dithered to the VIDC1 palette,
 * the error kept along a row pair and dropped every 16 pixels from xmin */
static void merged_vidc(const struct jimg *im, int sy, uint8_t *o0, uint8_t *o1, int xmin, int xmax)
{
    int ey = 0, eu = 0, ev = 0, xpos = 0;
    xmin &= ~1;
    int width = ((xmax + 1) & ~1) - xmin;
    const uint8_t *y0 = im->c[0].p + (size_t)sy * im->c[0].pw, *y1 = y0 + im->c[0].pw;
    for (int col = width >> 1, x = xmin; col > 0; col--, x += 2) {
        if (xpos % 16 == 0)
            ey = eu = ev = 0;
        int cb = sample(im, 1, x, sy), cr = sample(im, 2, x, sy);
        int luma[4] = { y1[x], y0[x], y0[x + 1], y1[x + 1] };
        for (int p = 0; p < 4; p++) {
            int Y = clamp255(luma[p] + ey), u = clamp255(cb + eu), v = clamp255(cr + ev);
            uint8_t c = yuv_to_pixel[((Y & 0xF8) << 5) | (u & 0xF0) | ((v & 0xF0) >> 4)];
            if (p == 1 || p == 2) {
                if (o0)
                    o0[x + ((p & 2) >> 1)] = c;
            } else if (o1) {
                o1[x + ((p & 2) >> 1)] = c;
            }
            ey = Y - (int)((pixel_to_yuv[c] >> 16) & 0xFF);
            eu = u - (int)((pixel_to_yuv[c] >> 8) & 0xFF);
            ev = v - (int)(pixel_to_yuv[c] & 0xFF);
        }
        xpos += 2;
    }
}

/* ---- the plot ------------------------------------------------------------------------------------------ */

struct jplot {
    struct sx_jsrc src;
    struct jimg *im;
    uint32_t data, len;
    uint8_t *rows;                      /* the image as the blitter reads it, top row first */
};

/* OS_ReadModeVariable -1 */
static uint32_t mode_var(uint32_t v)
{
    uint32_t r[10] = { 0xFFFFFFFFu, v };
    os_error *e;
    sx_swi(XOS_ReadModeVariable, r, &e);
    return r[2];
}

/* ColourTrans's 32K table for 32bpp data into the destination, or 0 */
static uint32_t table32k(void)
{
    struct sx_jstate *st = jstate();
    if (!st)
        return 0;
    uint32_t r[10] = { (6u << 27) | 1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, ros_addr(st->table),
                       0, 0, 0 };
    os_error *e;
    sx_swi(XColourTrans_SelectTable, r, &e);
    if (e || st->table[0] != 0x2E4B3233u || st->table[2] != 0x2E4B3233u)
        return 0;
    return st->table[1];
}

/* The destination's palette (ColourTrans_ReadPalette), &BBGGRR00 */
static int read_palette(uint32_t *pal)
{
    struct sx_jstate *st = jstate();
    uint32_t r[10] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 256 * 4, 0 };
    os_error *e;
    sx_swi(XColourTrans_ReadPalette, r, &e);
    uint32_t size = e ? 0 : r[3];
    if (size > 1024)
        size = 1024;
    memset(st->palette, 0, sizeof st->palette);
    uint32_t q[10] = { 0xFFFFFFFFu, 0xFFFFFFFFu, ros_addr(st->palette), size, 0 };
    sx_swi(XColourTrans_ReadPalette, q, &e);
    memcpy(pal, st->palette, sizeof st->palette);
    return (int)(size / 4);
}

/* palette_is_grey: 0 no, 1 grey, 2 grey and ascending */
static int palette_is_grey(const uint32_t *pal, int n)
{
    int ascending = 1;
    for (int i = 0; i < n; i++) {
        uint32_t e = pal[i];
        if (((e ^ (e >> 8)) & 0xFFFF00u) != 0)
            return 0;
        if (((e & 0xFF00) >> 8) != (uint32_t)i)
            ascending = 0;
    }
    return ascending ? 2 : 1;
}

enum {
    JOPT_GREY = 1, JOPT_DC_ONLY = 2, JOPT_DIFFUSE = 8, JOPT_OUT16 = 16, JOPT_8YUV = 32,
    JOPT_8DITHER = 64, JOPT_8GREY = 128,
};

/* Called by scaled.c when the plot's geometry is known: decode, and lay
 * out the pixels as the original's options would have them
 * (jpeg_decompressor_opts, jpeg_scan_file, jpeg_find_line, putscaled_compiler) */
static os_error *build(struct sx_jsrc *j, const struct sx_jgeom *g)
{
    struct jplot *jp = (struct jplot *)j;
    const struct sx_dest *d = g->d;
    uint32_t bpp = d->bpp;
    struct sx_jstate *st = jstate();
    int colourmap = (j->flags & 8) != 0;
    int printing = st && (st->intercept & 2);
    uint32_t dt = j->flags & 3;
    uint32_t pal[256];
    int opt = 0;

    /* save_xadd is xdiv + xmag, but save_yadd is ymag alone, so the y test
     * holds for any reduction */
    if (g->xmag * 6 <= g->xdiv && ((int64_t)g->ymag - g->ydiv) * 6 <= (int64_t)g->ydiv)
        opt |= JOPT_DC_ONLY;
    j->ttr = 0;
    if (!colourmap && bpp <= 8)
        j->ttr = table32k();
    if (!colourmap) {
        if (bpp < 4) {
            opt |= JOPT_GREY;
            if (!printing)
                j->ttr = 0;
        }
        if (bpp <= 8 && (dt & 2)) {
            opt |= JOPT_DIFFUSE;
            j->ttr = 0;
        }
        if (bpp == 8) {
            int grey = palette_is_grey(pal, read_palette(pal));
            if (grey)
                opt |= JOPT_GREY;
            if (grey == 2)
                opt = (opt | JOPT_8GREY) & ~JOPT_DIFFUSE;
        }
        if (!printing && (dt & 1)) {
            if ((dt & 2) && bpp < 16 && bpp == 4) {
                if (palette_is_grey(pal, read_palette(pal)))
                    opt |= JOPT_GREY;
            }
            if (bpp == 4 && !(dt & 2))
                opt |= JOPT_GREY;
            if (bpp == 8) {
                if ((dt & 2) && !(opt & JOPT_8GREY))
                    opt |= JOPT_8DITHER;
                else if (!(mode_var(0) & 0x80))
                    opt |= JOPT_8YUV;
            } else if (bpp == 16) {
                opt |= JOPT_OUT16;
            }
        }
    }
    if ((opt & JOPT_DIFFUSE) && bpp < 8)
        read_palette(pal);

    /* jpeg_scan_file's clipping box */
    int32_t xmin = (int32_t)g->in_x, xmax;
    uint64_t span = (uint64_t)g->xsize * g->xdiv / g->xmag;
    xmax = (int32_t)(g->in_x + 2 + span);
    if (xmax < 0 || span > 0x7FFFFFFF)
        xmax = (int32_t)j->w;
    if (opt & (JOPT_8DITHER | JOPT_8YUV)) {
        xmin -= 16;
        if (xmin < 0)
            xmin = 0;
        xmin &= ~15;
        xmax += 16;
    }

    uint32_t e = decoded(jp->data, jp->len, (opt & JOPT_DC_ONLY) != 0, &jp->im);
    if (e)
        return jerr(e);
    const struct jimg *im = jp->im;
    if (xmax > im->w)
        xmax = im->w;
    if (xmin > xmax)
        xmin = xmax;
    int merged = is_merged(im);
    if (!merged)
        opt &= ~(JOPT_OUT16 | JOPT_8YUV | JOPT_8GREY);
    /* putscaled_compiler's pixel formats.  Grey output (jopt_GREY, 24-bit
     * grey in 32-bit words) at 8bpp with no table has no conversion.  The
     * error diffusion flag took the table away, and then a grey palette
     * took the diffusion away again.  The original fails in convert_pixel's
     * assert, "fatal inconsistency" */
    int native = (opt & (JOPT_DIFFUSE | JOPT_8GREY | JOPT_OUT16 | JOPT_8YUV)) != 0;
    if (!native && (opt & JOPT_GREY) && bpp == 8 && !j->ttr)
        return jerr(E_JPEGFATAL);
    if (!(opt & JOPT_DIFFUSE))
        dt &= ~2u;
    if (opt & JOPT_OUT16)
        dt &= ~3u;
    j->dither_tc = dt;
    j->grey = (opt & JOPT_GREY) && !(opt & (JOPT_DIFFUSE | JOPT_8GREY | JOPT_OUT16 | JOPT_8YUV));
    j->grey_jpeg = im->cs == CS_GREY;

    ycc_tables();
    int band = im->max_v * 8, bands = (im->h + band - 1) / band;
    int w = im->w, pw = (w + 3) & ~3;
    /* the band buffer's rows as 32bpp &0BGR (or the merged outputs), a band at a time */
    int l2 = 5;
    if (opt & (JOPT_8DITHER | JOPT_8YUV | JOPT_8GREY))
        l2 = 3;
    else if (opt & JOPT_OUT16)
        l2 = 4;
    else if (opt & JOPT_DIFFUSE)
        l2 = 5;
    size_t stride = (size_t)pw << l2 >> 3;
    free(jp->rows);
    jp->rows = calloc((size_t)bands * band * stride + 16, 1);
    uint32_t *bandbuf = calloc((size_t)band * (pw + 4), 4);
    uint32_t **brow = calloc((size_t)band, sizeof *brow), **orow = calloc((size_t)band, sizeof *orow);
    uint32_t *obuf = calloc((size_t)band * (pw + 4), 4);
    if (!jp->rows || !bandbuf || !brow || !orow || !obuf) {
        free(bandbuf), free(brow), free(orow), free(obuf);
        return jerr(E_JPEGNOROOM);
    }
    if (opt & JOPT_8YUV)
        vidc_tables();
    for (int r = 0; r < band; r++)
        brow[r] = bandbuf + (size_t)r * (pw + 4), orow[r] = obuf + (size_t)r * (pw + 4);
    for (int bi = 0; bi < bands; bi++) {
        int y0 = bi * band;
        uint8_t *out = jp->rows + (size_t)y0 * stride;
        if (opt & (JOPT_OUT16 | JOPT_8YUV | JOPT_8GREY)) {
            for (int r = 0; r < band; r += 2) {
                int sy = y0 + r;
                if (opt & JOPT_OUT16)
                    merged16(im, d, sy, sy, (uint16_t *)(void *)(out + (size_t)r * stride),
                             (uint16_t *)(void *)(out + (size_t)(r + 1) * stride), xmin, xmax);
                else if (opt & JOPT_8GREY)      /* before 8YUV, as jinit_merged_upsampler */
                    for (int k = 0; k < 2; k++)
                        memcpy(out + (size_t)(r + k) * stride,
                               im->c[0].p + (size_t)(sy + k) * im->c[0].pw, (size_t)w);
                else
                    merged_vidc(im, sy, out + (size_t)r * stride, out + (size_t)(r + 1) * stride,
                                xmin, xmax);
            }
        } else {
            for (int r = 0; r < band; r++) {
                int sy = y0 + r;
                for (int x = 0; x < w + 4; x++)
                    brow[r][x] = x < w ? rgb_at(im, x, sy < im->c[0].ph ? sy : im->c[0].ph - 1) : 0;
                if (merged) {
                    /* h2v2_merged_upsample_32bpp starts at xmin, odd or not, and
                     * pairs its pixels from there with the chroma from xmin / 2:
                     * an odd xmin shifts the chroma a pixel */
                    const uint8_t *yr = im->c[0].p + (size_t)sy * im->c[0].pw;
                    for (int x = xmin; x < w; x++) {
                        int cx = (xmin >> 1) + ((x - xmin) >> 1), cy = sy >> 1;
                        int cb = im->c[1].p[(size_t)cy * im->c[1].pw + cx];
                        int cr = im->c[2].p[(size_t)cy * im->c[2].pw + cx];
                        int Y = yr[x];
                        brow[r][x] = (uint32_t)clamp255(Y + crr[cr]) |
                                     (uint32_t)clamp255(Y + (int)((cbg[cb] + crg[cr]) >> SCALEBITS)) << 8 |
                                     (uint32_t)clamp255(Y + cbb[cb]) << 16;
                    }
                }
            }
            if (opt & JOPT_DIFFUSE) {
                struct diffuse f = { j->ttr ? NULL : NULL, pal, 0, 0, 0 };
                uint32_t t = table32k();
                f.table = t ? ros_ptr(t) : NULL;
                int len = xmax - xmin;
                len = (len + 15) & ~15;
                if (len > w - xmin)
                    len = w - xmin;
                int x = xmin;
                while (len > 0) {
                    int bw = len >= 32 ? 16 : len;
                    diffuse_block(&f, brow, band, bw, x, orow);
                    len -= bw, x += bw;
                }
                for (int r = 0; r < band; r++) {
                    uint8_t *o = out + (size_t)r * stride;
                    for (int x2 = 0; x2 < w; x2++) {
                        if (l2 == 3)
                            o[x2] = (uint8_t)orow[r][x2];
                        else
                            ((uint32_t *)(void *)o)[x2] = orow[r][x2];
                    }
                }
            } else {
                for (int r = 0; r < band; r++)
                    memcpy(out + (size_t)r * stride, brow[r], (size_t)w * 4);
            }
        }
    }
    free(bandbuf), free(brow), free(orow), free(obuf);

    /* DC only, jpeg_find_line returns row (y & band-1) >> 3 of the band */
    if (opt & JOPT_DC_ONLY) {
        for (int y = 0; y < im->h; y++) {
            int sy = (y & ~(band - 1)) + ((y & (band - 1)) >> 3);
            if (sy != y)
                memcpy(jp->rows + (size_t)y * stride, jp->rows + (size_t)sy * stride, stride);
        }
    }
    j->pix = jp->rows;
    j->stride = stride;
    j->l2bpp = (uint32_t)l2;
    j->kind = opt & (JOPT_DIFFUSE | JOPT_OUT16 | JOPT_8YUV | JOPT_8GREY) ? SX_JNATIVE : SX_JRGB;
    if (j->kind == SX_JNATIVE)
        j->ttr = 0;
    return NULL;
}

/* JPEG_PlotScaled's work: R0 -> JPEG, R1, R2 x, y, R3 -> scale or 0, R4
 * length, R5 flags, R6 -> colour mapping descriptor */
static os_error *plot_scaled(uint32_t data, int32_t x, int32_t y, uint32_t scale, uint32_t len,
                             uint32_t flags, uint32_t map)
{
    if (flags & ~(0xFF0u | 8u | 3u))
        return sx_err(SX_BADFLAGS);
    struct jsrc src = { ros_ptr(data), data, 0, len, 0 };
    struct jdims d;
    int code = find_dims(&src, &d);
    if (code != JFID_OK)
        return dims_error(code);
    ws_claim(ws_needed(&d));
    struct jplot jp;
    memset(&jp, 0, sizeof jp);
    jp.data = data, jp.len = len;
    jp.src.w = (uint32_t)d.w, jp.src.h = (uint32_t)d.h;
    jp.src.flags = flags, jp.src.map = map;
    jp.src.build = build;
    jp.src.x = x, jp.src.y = y;
    if (scale)
        for (int i = 0; i < 4; i++)
            jp.src.scale[i] = ros_ld32(scale + 4u * (uint32_t)i);
    else
        jp.src.scale[0] = jp.src.scale[1] = jp.src.scale[2] = jp.src.scale[3] = 1;
    os_error *e = sx_scaled_jpeg(&jp.src);
    free(jp.rows);
    return e;
}

/* The file calls: the file loaded into the RMA, as the original does */
static os_error *with_file(uint32_t name, uint8_t **buf, uint32_t *len)
{
    uint32_t r[10] = { 23, name };
    os_error *e;
    sx_swi(XOS_File, r, &e);
    if (e)
        return e;
    if (r[0] == 0) {
        char n[256];
        size_t i = 0;
        for (; i < sizeof n - 1 && ros_ld8(name + (uint32_t)i) >= 32; i++)
            n[i] = (char)ros_ld8(name + (uint32_t)i);
        n[i] = 0;
        return ros_error(0xD6, "File '%s' not found", n);   /* the original's NoFile, the name as given */
    }
    void *b;
    if (xos_module_claim(r[4] ? r[4] : 4, &b))
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    uint32_t l[10] = { 16, name, ros_addr(b), 0 };
    sx_swi(XOS_File, l, &e);
    if (e) {
        xos_module_free(b);
        return e;
    }
    *buf = b, *len = r[4];
    return NULL;
}

/* JPEG_PlotTransformed's work: a scale, never a rotation */
static os_error *plot_transformed(uint32_t data, uint32_t flags, uint32_t blk, uint32_t len,
                                  uint32_t map)
{
    if (flags & ~(0x1FE0u | 16u | 1u | 2u | 4u))
        return sx_err(SX_BADFLAGS);
    uint32_t sc[4];
    int32_t x, y;
    if (!(flags & 1)) {
        int32_t a = (int32_t)ros_ld32(blk), b = (int32_t)ros_ld32(blk + 4);
        int32_t c = (int32_t)ros_ld32(blk + 8), dd = (int32_t)ros_ld32(blk + 12);
        if (b != 0 || c != 0 || a < 0 || dd < 0)
            return jerr(E_BADJPEGPLOT);
        uint32_t xeig = mode_var(4), yeig = mode_var(5);
        struct jsrc src = { ros_ptr(data), data, 0, len, 0 };
        struct jdims d;
        memset(&d, 0, sizeof d);
        find_dims(&src, &d);
        uint32_t xo = 180u >> xeig, yo = 180u >> yeig;
        sc[0] = (uint32_t)a, sc[1] = (uint32_t)dd;
        sc[2] = xo ? (d.density >> 16 << 16) / xo : 0;
        sc[3] = yo ? ((d.density & 0xFFFF) << 16) / yo : 0;
        x = (int32_t)ros_ld32(blk + 16) >> 8, y = (int32_t)ros_ld32(blk + 20) >> 8;
    } else {
        uint32_t X0 = ros_ld32(blk), Y0 = ros_ld32(blk + 4), X1 = ros_ld32(blk + 8);
        uint32_t Y1 = ros_ld32(blk + 12), X2 = ros_ld32(blk + 16), Y2 = ros_ld32(blk + 20);
        uint32_t X3 = ros_ld32(blk + 24), Y3 = ros_ld32(blk + 28);
        if (X0 != X3 || Y0 != Y1 || Y2 != Y3 || X1 != X2 || (int32_t)X0 > (int32_t)X2 ||
            (int32_t)Y3 > (int32_t)Y0)
            return jerr(E_BADJPEGPLOT);
        uint32_t dw = ((X2 - X0) >> 8) >> mode_var(4), dh = ((Y0 - Y3) >> 8) >> mode_var(5);
        struct jsrc src = { ros_ptr(data), data, 0, len, 0 };
        struct jdims d;
        memset(&d, 0, sizeof d);
        find_dims(&src, &d);
        sc[0] = dw, sc[1] = dh, sc[2] = (uint32_t)d.w, sc[3] = (uint32_t)d.h;
        x = (int32_t)X0 >> 8, y = (int32_t)Y3 >> 8;
    }
    void *b;
    if (xos_module_claim(16, &b))
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(b, sc, 16);
    os_error *e = plot_scaled(data, x, y, ros_addr(b), len, flags >> 1, map);
    xos_module_free(b);
    return e;
}

/* ---- the SWIs ------------------------------------------------------------------------------------ */

static void done(struct ros_cpu *s, os_error *e)
{
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

/* The printer driver's intercept, when on: the plot handed to it */
static int intercepted(struct ros_cpu *s, uint32_t reason)
{
    struct sx_jstate *st = jstate();
    if (!st || !(st->intercept & 1))
        return 0;
    struct ros_cpu c = *s;
    c.r[8] = reason;
    ros_swi(&c, 0xA015Du);              /* XPDriver_JPEGSWI */
    s->r[0] = c.r[0];
    s->v = c.v;
    return 1;
}

void ros_thunk_JPEG_Info(struct ros_cpu *s)
{
    uint32_t r[10];
    memcpy(r, s->r, sizeof r);
    os_error *e = info(s, 0);
    if (e) {
        memcpy(s->r, r, sizeof r);
        done(s, e);
        return;
    }
    if (!(r[0] & 1))
        s->r[2] = r[2], s->r[3] = r[3];
    done(s, NULL);
}

void ros_thunk_JPEG_FileInfo(struct ros_cpu *s)
{
    uint32_t r[10];
    memcpy(r, s->r, sizeof r);
    os_error *e = info(s, 1);
    if (e) {
        memcpy(s->r, r, sizeof r);
        done(s, e);
        return;
    }
    if (!(r[0] & 1))
        s->r[2] = r[2], s->r[3] = r[3];
    done(s, NULL);
}

void ros_thunk_JPEG_PlotScaled(struct ros_cpu *s)
{
    if (intercepted(s, 2))
        return;
    done(s, plot_scaled(s->r[0], (int32_t)s->r[1], (int32_t)s->r[2], s->r[3], s->r[4], s->r[5],
                        s->r[6]));
}

void ros_thunk_JPEG_PlotFileScaled(struct ros_cpu *s)
{
    if (s->r[4] & ~(0xFF0u | 8u | 3u)) {
        done(s, sx_err(SX_BADFLAGS));
        return;
    }
    if (intercepted(s, 3))
        return;
    uint8_t *buf;
    uint32_t len;
    os_error *e = with_file(s->r[0], &buf, &len);
    if (!e) {
        e = plot_scaled(ros_addr(buf), (int32_t)s->r[1], (int32_t)s->r[2], s->r[3], len, s->r[4],
                        s->r[5]);
        xos_module_free(buf);
    }
    done(s, e);
}

void ros_thunk_JPEG_PlotTransformed(struct ros_cpu *s)
{
    if (intercepted(s, 4))
        return;
    done(s, plot_transformed(s->r[0], s->r[1], s->r[2], s->r[3], s->r[4]));
}

void ros_thunk_JPEG_PlotFileTransformed(struct ros_cpu *s)
{
    if (s->r[1] & ~(0x1FE0u | 16u | 1u | 2u | 4u)) {
        done(s, sx_err(SX_BADFLAGS));
        return;
    }
    if (intercepted(s, 5))
        return;
    uint8_t *buf;
    uint32_t len;
    os_error *e = with_file(s->r[0], &buf, &len);
    if (!e) {
        e = plot_transformed(ros_addr(buf), s->r[1], s->r[2], len, s->r[3]);
        xos_module_free(buf);
    }
    done(s, e);
}

void ros_thunk_JPEG_PDriverIntercept(struct ros_cpu *s)
{
    struct sx_jstate *st = jstate();
    uint32_t old = st ? st->intercept : 0;
    if (st)
        st->intercept = s->r[0] & 3;
    s->r[0] = old;
    s->v = 0;
}

/* The original's BadSWI: its message token's %0 is never filled in */
os_error *sx_jpeg_bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x110, "SWI value out of range for module %%0");
}
