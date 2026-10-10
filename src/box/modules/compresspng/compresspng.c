/* Copyright 2019 RISC OS Open Ltd
 * Copyright 2020 RISC OS Open Ltd
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
 * This file is a reimplementation in C of RISC OS Open's CompressPNG module
 * (Sources/Video/Render/CompressPNG: c.compresspng, c.memory, c.module, h.CompressPNG,
 * h.CompressPNG_int).
 */

/* compresspng.c -- CompressPNG (Video/Render/CompressPNG, 0.07, 08 Jul
 * 2023), as a native module. Its SWIs are its c/compresspng's, call for
 * call. They run over libpng 1.6 (built by deps/build-imagelibs.sh). The
 * zlib code is in /init. It is not reached through the ZLib module's SWIs
 * (its s/zlibswi).
 *
 * The memory is the original's (c/memory). Each compression has a dynamic
 * area of its own, called "CompressPNG", of at most 32MB, with a heap in
 * it. The tag that a program is given is a block in that heap. Everything
 * libpng allocates is in the heap too. The areas are listed with their
 * tasks. They go when a task ends without finishing (Service_WimpCloseDown)
 * and when the module goes.
 *
 * Two features of RISC OS's C show in the interface, and they are kept.
 * First, a palette is a list of words, with red in the low byte. It is not
 * a list of three-byte png_colors, because Norcroft pads that structure to
 * four bytes. Second, a gamma parameter is a double in FPA's word order,
 * with the high word first.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <png.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "compresspng.h"

#define TAG_MAGIC 0x4D474E50u           /* "PNGM" as bytes. See _tag_valid in h/compresspng_int. */
#define MAX_AREA_SIZE (32u * 1024 * 1024)
#define FILETYPE_PNG 0xB60u
#define BOUNCE 4096u

/* The error codes of h/module, as offsets from ErrorBase_CompressPNG. */
enum { E_BADTAG, E_NOMEM, E_INITX2, E_INITX, E_COMMENTX2, E_COMMENTX, E_MANYROWS,
       E_OPTFAIL2, E_OPTFAIL, E_FEWROWS, E_BADBLOCK };

/* Where the PNG goes. */
enum { TO_BUFFER, TO_SIZE, TO_FILE };

/* A compression's dynamic area (c/memory's cpngmem). It is an RMA block. */
struct cpngmem {
    uint32_t taskh;
    uint32_t area;
    uint32_t base;                      /* the area's base, where its heap is */
    uint32_t next;                      /* the next cpngmem, or 0 */
};

/* The tag (h/compresspng_int's png_opt). It is in the area's heap. */
struct png_opt {
    uint32_t magic;
    int destination;
    int error_state;
    uint32_t dest_filename;             /* the file name, in the heap, or 0 */
    uint32_t handle;                    /* the file's handle */
    uint32_t dest_buffer;
    uint32_t dest_length;
    uint32_t write_offset;
    int width, palette_bits, rowdup, info_written, filler, height, current_row;
    int pixel_size;                     /* bytes per pixel in the rows a program gives */
    png_structp png_ptr;
    png_infop info_ptr;
    int interlaced;
    int stride;
    png_bytep full_source;
    png_bytepp row_ptrs;
    struct cpngmem *mem;
    uint8_t *bounce;                    /* a file's writes go through this, in the area: */
                                        /* libpng writes some data from its own stack */
    char pending[256];                  /* a copy of libpng's error (png_pending_error) */
    int has_pending;
};

/* The module's workspace. */
struct cp_workspace {
    uint32_t memlist;                   /* the first cpngmem in the list, or 0 */
    uint32_t pagesize;
    char zlib_swi[16];                  /* "ZLib_Compress", for OS_SWINumberFromString */
    char area_name[16];
    char error_text[256];               /* not used: report_error builds its text in a local buffer */
};

static struct cp_workspace *ws(void)
{
    return ros_ptr(ros_ld32(compresspng_module.private_word));
}

static os_error *swi(uint32_t number, uint32_t *r)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 9 * 4);
    ros_swi(&c, number);
    memcpy(r, c.r, 9 * 4);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static int valid(uint32_t a, uint32_t len)
{
    if (len == 0)
        return 1;
    if (a == 0 || a + len < a)
        return 0;
    int invalid = 1;
    xos_validate_address(a, a + len, &invalid);
    return !invalid;
}

/* Checks a string that a program gave. It must be readable up to its
 * terminator, which is a zero byte, as C reads it. */
static int valid_string(uint32_t a)
{
    if (!valid(a, 1))
        return 0;
    for (uint32_t p = a;; p++) {
        if ((p & 0xFFFu) == 0 && !valid(p, 1))
            return 0;
        if (ros_ld8(p) == 0)
            return 1;
    }
}

static os_error *abort_at(uint32_t addr)
{
    return ros_error(0x80000002u, "Internal error: abort on data transfer at &%08X", addr);
}

/* ---- Resources.CompressPNG.Messages --------------------------------------------------- */

static const char *message(const char *token)
{
    static const char *const m[][2] = {
        { "badtag", "Invalid tag pointer passed to CompressPNG_ SWI" },
        { "nomem", "There is not enough free memory to perform the requested operation" },
        { "filefail", "Unable to create output file %0" },
        { "pnginitx", "Failed to initialise PNG output" },
        { "pnginitx2", "Failed to initialise PNG output: %0" },
        { "commentx", "Unable to add text comment to PNG" },
        { "commentx2", "Unable to add text comment to PNG: %0" },
        { "manyrows", "Too many rows have been output for this image" },
        { "fewrows", "Not enough rows have been output for this image" },
        { "optfail", "Failed to output PNG data" },
        { "optfail2", "Failed to output PNG data: %0" },
        { "buffer", "The supplied buffer is too small to hold the PNG image" },
        { "writefail", "Failed to write PNG data to file" },
        { "noalfapal", "Alpha channels are not supported for palettised images" },
        { "badblock", "Memory block not known" },
        { "badbits", "Palette data may only be 2, 4, 8, 16 or 256 entries" },
        { "badtrns", "Transparency count exceeds palette size" },
        { "nozlib", "ZLib module is not loaded" },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!strcmp(m[i][0], token))
            return m[i][1];
    return token;
}

/* c/module's report_error. It gives the token's text, with the argument
 * substituted for %0. */
static os_error *report_error(int code, const char *token, const char *arg)
{
    char text[252];
    const char *t = message(token);
    size_t n = 0;
    for (; *t && n + 1 < sizeof text; t++) {
        if (t[0] == '%' && t[1] == '0') {
            for (const char *a = arg ? arg : ""; *a && n + 1 < sizeof text; a++)
                text[n++] = *a;
            t++;
        } else {
            text[n++] = *t;
        }
    }
    text[n] = 0;
    return ros_error(COMPRESSPNG_ERRBASE + (uint32_t)code, "%s", text);
}

/* ---- c/memory ------------------------------------------------------------------------- */

static os_error *mem_create_area(struct cpngmem **out)
{
    struct cp_workspace *w = ws();
    struct cpngmem *m;
    if (xos_module_claim(sizeof *m, (void **)&m))
        return report_error(E_NOMEM, "nomem", NULL);
    memset(m, 0, sizeof *m);
    uint32_t r[9] = { 0, 0xFFFFFFFFu, w->pagesize, 0xFFFFFFFFu, 0x80u, MAX_AREA_SIZE, 0, 0,
                      ros_addr(w->area_name) };
    os_error *e = swi(XOS_DynamicArea, r);
    if (e) {
        xos_module_free(m);
        return e;
    }
    m->area = r[1];
    m->base = r[3];
    uint32_t t[9] = { 5 };
    m->taskh = swi(XWimp_ReadSysInfo, t) ? 0 : t[0];
    m->next = w->memlist;
    w->memlist = ros_addr(m);
    uint32_t h[9] = { 0, m->base, 0, w->pagesize };
    swi(XOS_Heap, h);
    *out = m;
    return NULL;
}

static void mem_remove_area(uint32_t area)
{
    uint32_t r[9] = { 1, area };
    swi(XOS_DynamicArea, r);
}

static os_error *mem_free_area(struct cpngmem *m)
{
    struct cp_workspace *w = ws();
    uint32_t a = ros_addr(m), *p = &w->memlist;
    while (*p && *p != a)
        p = &((struct cpngmem *)ros_ptr(*p))->next;
    if (!*p)
        return report_error(E_BADBLOCK, "badblock", NULL);
    *p = m->next;
    uint32_t r[9] = { 1, m->area };
    os_error *e = swi(XOS_DynamicArea, r);
    xos_module_free(m);
    return e;
}

/* Frees every area of a task (taskh), or all of the areas if all is set.
 * The module's finalisation passes taskh 0 with all set. */
static void mem_free_task(uint32_t taskh, int all)
{
    struct cp_workspace *w = ws();
    uint32_t *p = &w->memlist;
    while (*p) {
        struct cpngmem *m = ros_ptr(*p);
        if (all || m->taskh == taskh) {
            *p = m->next;
            mem_remove_area(m->area);
            xos_module_free(m);
        } else {
            p = &m->next;
        }
    }
}

/* Allocates a block from the area's heap. The area is grown if it must be. */
static void *mem_alloc(const struct cpngmem *m, uint32_t length)
{
    uint32_t r[9] = { 2, m->base, 0, length };
    if (!swi(XOS_Heap, r) && r[2])
        return ros_ptr(r[2]);
    uint32_t g[9] = { 2, m->area };
    if (swi(XOS_DynamicArea, g))
        return NULL;
    uint32_t pagesize = ws()->pagesize;
    uint32_t size = g[2], want = (size + length + 16 + pagesize - 1) & ~(pagesize - 1);
    uint32_t c[9] = { m->area, want - size };
    if (swi(XOS_ChangeDynamicArea, c))
        return NULL;
    uint32_t x[9] = { 5, m->base, 0, c[1] };
    if (swi(XOS_Heap, x))
        return NULL;
    uint32_t again[9] = { 2, m->base, 0, length };
    if (swi(XOS_Heap, again) || !again[2])
        return NULL;
    return ros_ptr(again[2]);
}

static void mem_free(const struct cpngmem *m, void *block)
{
    if (!block)
        return;
    uint32_t r[9] = { 3, m->base, ros_addr(block) };
    swi(XOS_Heap, r);
}

static png_voidp png_mem_alloc(png_structp png, png_alloc_size_t size)
{
    if (size > 0x7FFFFFF0u)
        return NULL;
    return mem_alloc(png_get_mem_ptr(png), (uint32_t)size);
}

static void png_mem_free(png_structp png, png_voidp p)
{
    mem_free(png_get_mem_ptr(png), p);
}

/* ---- libpng's callbacks ----------------------------------------------------------------- */

/* An error. The message is kept and control goes back to the SWI's setjmp,
 * as png_riscos_error does. */
static void png_riscos_error(png_structp png, png_const_charp message)
{
    struct png_opt *opt = png_get_error_ptr(png);
    snprintf(opt->pending, sizeof opt->pending, "%s", message);
    opt->has_pending = 1;
    png_longjmp(png, 1);
}

static void png_riscos_warning(png_structp png, png_const_charp message)
{
    (void)png, (void)message;
}

static void png_user_write_data(png_structp png, png_bytep data, size_t length)
{
    struct png_opt *opt = png_get_io_ptr(png);
    switch (opt->destination) {
    case TO_BUFFER:
        if ((uint64_t)opt->write_offset + length > opt->dest_length)
            png_error(png, message("buffer"));
        memcpy(ros_ptr(opt->dest_buffer + opt->write_offset), data, length);
        opt->write_offset += (uint32_t)length;
        break;
    case TO_SIZE:
        opt->write_offset += (uint32_t)length;
        break;
    default:
        for (size_t done = 0; done < length;) {
            uint32_t n = length - done > BOUNCE ? BOUNCE : (uint32_t)(length - done);
            memcpy(opt->bounce, data + done, n);
            uint32_t r[9] = { 2, opt->handle, ros_addr(opt->bounce), n };
            os_error *e = swi(XOS_GBPB, r);
            if (e || r[3])
                png_error(png, e ? e->errmess : message("writefail"));
            done += n;
        }
        opt->write_offset += (uint32_t)length;
        break;
    }
}

static void png_user_flush_data(png_structp png)
{
    (void)png;
}

/* ---- the output file --------------------------------------------------------------------- */

static void close_file(struct png_opt *opt)
{
    uint32_t r[9] = { 0, opt->handle };
    swi(XOS_Find, r);
    opt->handle = 0;
}

static void fclose_and_remove(struct png_opt *opt)
{
    close_file(opt);
    if (opt->dest_filename) {
        uint32_t r[9] = { 6, opt->dest_filename };
        swi(XOS_File, r);
    }
}

static os_error *fclose_and_settype(struct png_opt *opt)
{
    close_file(opt);
    uint32_t r[9] = { 18, opt->dest_filename, FILETYPE_PNG };
    os_error *e = swi(XOS_File, r);
    if (e) {
        e = ros_error(e->errnum, "%s", e->errmess);
        uint32_t d[9] = { 6, opt->dest_filename };
        swi(XOS_File, d);
    }
    return e;
}

/* ---- the SWIs ------------------------------------------------------------------------------- */

/* Checks a tag, as compresspng_tag_valid does. */
static struct png_opt *tag_of(uint32_t tag)
{
    if (!valid(tag, sizeof(struct png_opt)) || (tag & 3))
        return NULL;
    struct png_opt *opt = ros_ptr(tag);
    return opt->magic == TAG_MAGIC ? opt : NULL;
}

static int bit_depth(int32_t colours)
{
    switch (colours) {
    case 2: return 1;
    case 4: return 2;
    case 16: return 4;
    case 256: return 8;
    default: return -1;
    }
}

/* The parameter block (h/CompressPNG's compresspng_str). It is five words,
 * then parameters of twelve bytes each, up to one with a type of 0. */
#define BLK_WIDTH  0u
#define BLK_HEIGHT 4u
#define BLK_XDPI   8u
#define BLK_YDPI   12u
#define BLK_FLAGS  16u
#define BLK_PARAMS 20u
enum { P_END, P_TEXT, P_GAMMA, P_COMPRESSION, P_INTERLACE, P_PALETTE, P_TRANSPARENCY };

/* Reads the double at address a, which is in FPA's word order. */
static double fpa_double(uint32_t a)
{
    uint64_t bits = (uint64_t)ros_ld32(a) << 32 | ros_ld32(a + 4);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

/* CompressPNG_Start: R0 -> the buffer or the file name (or 0 to ask only for
 * the size), R1 is the buffer's size and R2 -> the parameter block. On exit
 * R0 is the tag. */
static os_error *start(uint32_t *r)
{
    uint32_t dest = r[0], destlen = r[1], block = r[2];
    struct cp_workspace *w = ws();

    /* The ZLib module must be loaded, as the original's zlib calls need. */
    uint32_t n[9] = { 0, ros_addr(w->zlib_swi) };
    if (swi(XOS_SWINumberFromString, n))
        return report_error(E_INITX2, "pnginitx2", message("nozlib"));

    /* Check the block: its five words and its parameters, to the end. */
    if (!valid(block, BLK_PARAMS + 4))
        return abort_at(block);
    uint32_t nparams = 0;
    for (;; nparams++) {
        uint32_t p = block + BLK_PARAMS + 12 * nparams;
        if (!valid(p, 4))
            return abort_at(p);
        if (ros_ld32(p) == P_END)
            break;
        if (!valid(p, 12))
            return abort_at(p);
    }
    int32_t width = (int32_t)ros_ld32(block + BLK_WIDTH), height = (int32_t)ros_ld32(block + BLK_HEIGHT);
    int32_t x_dpi = (int32_t)ros_ld32(block + BLK_XDPI), y_dpi = (int32_t)ros_ld32(block + BLK_YDPI);
    uint32_t flags = ros_ld32(block + BLK_FLAGS);
    if ((flags & COMPRESSPNG_TO_FILE) && !valid_string(dest))
        return abort_at(dest);

    struct cpngmem *mem;
    os_error *volatile e = mem_create_area(&mem);
    if (e)
        return e;
    struct png_opt *opt = mem_alloc(mem, sizeof *opt);
    if (!opt) {
        mem_free_area(mem);
        return report_error(E_NOMEM, "nomem", NULL);
    }
    memset(opt, 0, sizeof *opt);
    opt->magic = TAG_MAGIC;
    opt->mem = mem;
    opt->png_ptr = png_create_write_struct_2(PNG_LIBPNG_VER_STRING, opt, png_riscos_error,
                                             png_riscos_warning, mem, png_mem_alloc, png_mem_free);
    if (!opt->png_ptr) {
        mem_free(mem, opt);
        mem_free_area(mem);
        return report_error(E_NOMEM, "nomem", NULL);
    }
    opt->info_ptr = png_create_info_struct(opt->png_ptr);
    if (!opt->info_ptr) {
        png_destroy_write_struct(&opt->png_ptr, NULL);
        mem_free(mem, opt);
        mem_free_area(mem);
        return report_error(E_NOMEM, "nomem", NULL);
    }

    /* Set up the destination. */
    if (flags & COMPRESSPNG_TO_FILE) {
        opt->destination = TO_FILE;
        size_t len = strlen(ros_ptr(dest));
        char *name = mem_alloc(mem, (uint32_t)len + 1);
        if (!name) {
            e = report_error(E_NOMEM, "nomem", NULL);
            goto abort_png;
        }
        memcpy(name, ros_ptr(dest), len + 1);
        opt->dest_filename = ros_addr(name);
        opt->bounce = mem_alloc(mem, BOUNCE);
        if (!opt->bounce) {
            e = report_error(E_NOMEM, "nomem", NULL);
            goto abort_png;
        }
        uint32_t f[9] = { 0x83, opt->dest_filename };      /* fopen(, "wb") */
        e = swi(XOS_Find, f);
        if (e || f[0] == 0) {
            e = e ? ros_error(e->errnum, "%s", e->errmess) : report_error(E_INITX, "pnginitx", NULL);
            png_destroy_write_struct(&opt->png_ptr, &opt->info_ptr);
            mem_free(mem, name);
            mem_free(mem, opt);
            mem_free_area(mem);
            return e;
        }
        opt->handle = f[0];
    } else if (destlen == 0) {
        opt->destination = TO_SIZE;
    } else {
        opt->destination = TO_BUFFER;
        if (!valid(dest, destlen)) {
            e = abort_at(dest);
            goto abort_png;
        }
        opt->dest_buffer = dest;
        opt->dest_length = destlen;
    }
    png_set_write_fn(opt->png_ptr, opt, png_user_write_data, png_user_flush_data);

    if (setjmp(png_jmpbuf(opt->png_ptr)))
        goto abort_png;

    /* Rows are repeated where the pixels are taller than they are wide. */
    opt->rowdup = y_dpi > 0 ? x_dpi / y_dpi : 1;
    if (opt->rowdup < 1)
        opt->rowdup = 1;
    opt->height = height * opt->rowdup;

    volatile int png_colour, pixel_size;
    if (flags & COMPRESSPNG_GREYSCALE) {
        png_colour = flags & COMPRESSPNG_HAS_ALPHA ? PNG_COLOR_TYPE_GRAY_ALPHA : PNG_COLOR_TYPE_GRAY;
        pixel_size = flags & COMPRESSPNG_HAS_ALPHA ? 2 : 1;
    } else {
        png_colour = flags & COMPRESSPNG_HAS_ALPHA ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB;
        pixel_size = flags & COMPRESSPNG_HAS_ALPHA ? 4 : 3;
    }
    if ((flags & COMPRESSPNG_SKIP_ALPHA) && !(flags & COMPRESSPNG_HAS_ALPHA)) {
        opt->filler = 1;
        pixel_size += 1;
    }
    opt->palette_bits = 8;
    volatile int png_interlace = PNG_INTERLACE_NONE;

    /* Handle the palette first. */
    for (uint32_t i = 0; i < nparams; i++) {
        uint32_t p = block + BLK_PARAMS + 12 * i;
        if (ros_ld32(p) != P_PALETTE)
            continue;
        if (flags & (COMPRESSPNG_HAS_ALPHA | COMPRESSPNG_SKIP_ALPHA)) {
            snprintf(opt->pending, sizeof opt->pending, "%s", message("noalfapal"));
            opt->has_pending = 1;
            goto abort_png;
        }
        int32_t size = (int32_t)ros_ld32(p + 4);
        opt->palette_bits = bit_depth(size);
        if (opt->palette_bits == -1) {
            snprintf(opt->pending, sizeof opt->pending, "%s", message("badbits"));
            opt->has_pending = 1;
            goto abort_png;
        }
        png_colour = PNG_COLOR_TYPE_PALETTE;
        pixel_size = 1;
        uint32_t data = ros_ld32(p + 8);
        if (!valid(data, (uint32_t)size * 4)) {
            e = abort_at(data);
            goto abort_png;
        }
        png_color pal[256];
        for (int32_t k = 0; k < size; k++) {
            uint32_t word = ros_ld32(data + 4u * (uint32_t)k);
            pal[k].red = (png_byte)word;
            pal[k].green = (png_byte)(word >> 8);
            pal[k].blue = (png_byte)(word >> 16);
        }
        png_set_PLTE(opt->png_ptr, opt->info_ptr, pal, size);
    }

    /* There are 3936/100 dots per metre for each dot per inch. The x
     * resolution is used in both directions, because the rows are repeated
     * instead. */
    png_uint_32 ppm = (png_uint_32)((int32_t)((uint32_t)x_dpi * 3936u) / 100);
    png_set_pHYs(opt->png_ptr, opt->info_ptr, ppm, ppm, PNG_RESOLUTION_METER);

    for (uint32_t i = 0; i < nparams; i++) {
        uint32_t p = block + BLK_PARAMS + 12 * i;
        switch (ros_ld32(p)) {
        case P_TEXT: {
            uint32_t key = ros_ld32(p + 4), value = ros_ld32(p + 8);
            if (!valid_string(key)) {
                e = abort_at(key);
                goto abort_png;
            }
            if (!valid_string(value)) {
                e = abort_at(value);
                goto abort_png;
            }
            png_text comment;
            memset(&comment, 0, sizeof comment);
            comment.compression = PNG_TEXT_COMPRESSION_NONE;
            comment.key = ros_ptr(key);
            comment.text = ros_ptr(value);
            png_set_text(opt->png_ptr, opt->info_ptr, &comment, 1);
            break;
        }
        case P_GAMMA:
            png_set_gAMA_fixed(opt->png_ptr, opt->info_ptr, (png_fixed_point)(fpa_double(p + 4) * 100000));
            break;
        case P_COMPRESSION:
            png_set_compression_level(opt->png_ptr, (int)ros_ld32(p + 4));
            break;
        case P_INTERLACE:
            if (ros_ld32(p + 4) == 1) {
                opt->interlaced = 1;
                png_interlace = PNG_INTERLACE_ADAM7;
                opt->stride = pixel_size * width;
                uint32_t rows = (uint32_t)height * (uint32_t)opt->rowdup;
                /* The sizes are worked out in 64 bits. In 32 bits the
                 * product of a row's bytes and the rows wraps for a large
                 * image. The block is then too small for the rows written
                 * into it, and the pointers made below run past it. A width
                 * or height that is not positive is left for png_set_IHDR to
                 * refuse. */
                if (width > 0 && height > 0) {
                    uint64_t table_bytes = (uint64_t)sizeof(png_bytep) * rows;
                    uint64_t image_bytes = (uint64_t)(uint32_t)opt->stride * rows;
                    if (table_bytes > 0x7FFFFFF0u || image_bytes > 0x7FFFFFF0u) {
                        e = report_error(E_NOMEM, "nomem", NULL);
                        goto abort_png;
                    }
                    opt->row_ptrs = mem_alloc(mem, (uint32_t)table_bytes);
                    if (opt->row_ptrs)
                        opt->full_source = mem_alloc(mem, (uint32_t)image_bytes);
                    if (!opt->row_ptrs || !opt->full_source) {
                        e = report_error(E_NOMEM, "nomem", NULL);
                        goto abort_png;
                    }
                    for (uint32_t row = 0; row < rows; row++)
                        opt->row_ptrs[row] = opt->full_source + (size_t)row * (uint32_t)opt->stride;
                }
            }
            break;
        case P_TRANSPARENCY: {
            int32_t size = (int32_t)ros_ld32(p + 4);
            if (size > (1 << opt->palette_bits)) {
                snprintf(opt->pending, sizeof opt->pending, "%s", message("badtrns"));
                opt->has_pending = 1;
                goto abort_png;
            }
            uint32_t data = ros_ld32(p + 8);
            if (size > 0 && !valid(data, (uint32_t)size)) {
                e = abort_at(data);
                goto abort_png;
            }
            png_set_tRNS(opt->png_ptr, opt->info_ptr, ros_ptr(data), size, NULL);
            break;
        }
        default:
            break;
        }
    }

    opt->pixel_size = pixel_size;
    png_set_IHDR(opt->png_ptr, opt->info_ptr, (png_uint_32)width, (png_uint_32)opt->height,
                 opt->palette_bits, png_colour, png_interlace, PNG_COMPRESSION_TYPE_BASE,
                 PNG_FILTER_TYPE_BASE);
    opt->width = width;
    r[0] = ros_addr(opt);
    return NULL;

abort_png:
    if (opt->destination == TO_FILE && opt->handle)
        fclose_and_remove(opt);
    mem_free(mem, opt->bounce);
    if (opt->dest_filename)
        mem_free(mem, ros_ptr(opt->dest_filename));
    if (opt->info_ptr)
        png_free_data(opt->png_ptr, opt->info_ptr, PNG_FREE_ALL, -1);
    if (opt->png_ptr)
        png_destroy_write_struct(&opt->png_ptr, &opt->info_ptr);
    if (opt->row_ptrs)
        mem_free(mem, opt->row_ptrs);
    if (opt->full_source)
        mem_free(mem, opt->full_source);
    if (opt->has_pending)
        e = report_error(E_INITX2, "pnginitx2", opt->pending);
    else if (!e)
        e = report_error(E_INITX, "pnginitx", NULL);
    else
        e = ros_error(e->errnum, "%s", e->errmess);
    opt->magic = 0;
    mem_free(mem, opt);
    mem_free_area(mem);
    return e;
}

/* CompressPNG_Comment: R0 is the tag, R1 -> the key and R2 -> the value. */
static os_error *comment(uint32_t *r, struct png_opt *opt)
{
    if (!valid_string(r[1]))
        return abort_at(r[1]);
    if (!valid_string(r[2]))
        return abort_at(r[2]);
    if (setjmp(png_jmpbuf(opt->png_ptr))) {
        os_error *e = opt->has_pending ? report_error(E_COMMENTX2, "commentx2", opt->pending)
                                       : report_error(E_COMMENTX, "commentx", NULL);
        opt->has_pending = 0;
        opt->error_state = 1;
        return e;
    }
    png_text text;
    memset(&text, 0, sizeof text);
    text.compression = PNG_TEXT_COMPRESSION_NONE;
    text.key = ros_ptr(r[1]);
    text.text = ros_ptr(r[2]);
    png_set_text(opt->png_ptr, opt->info_ptr, &text, 1);
    return NULL;
}

/* CompressPNG_WriteLine: R0 is the tag and R1 -> the row. A palettised row
 * of fewer than eight bits per pixel is packed where it lies, in the
 * caller's buffer, as the original packs it. */
static os_error *writeline(uint32_t *r, struct png_opt *opt)
{
    uint32_t row = r[1];
    uint64_t row_bytes = (uint64_t)(uint32_t)opt->width * (uint32_t)opt->pixel_size;
    if (row_bytes > 0xFFFFFFFFu || !valid(row, (uint32_t)row_bytes))
        return abort_at(row);
    if (setjmp(png_jmpbuf(opt->png_ptr))) {
        os_error *e = opt->has_pending ? report_error(E_OPTFAIL2, "optfail2", opt->pending)
                                       : report_error(E_OPTFAIL, "optfail", NULL);
        opt->has_pending = 0;
        opt->error_state = 1;
        return e;
    }
    if (!opt->info_written) {
        png_write_info(opt->png_ptr, opt->info_ptr);
        opt->info_written = 1;
        if (opt->filler)
            png_set_filler(opt->png_ptr, 0, PNG_FILLER_AFTER);
    }
    uint8_t *rowbuffer = ros_ptr(row);
    if (opt->palette_bits < 8) {
        int pixmask = (1 << opt->palette_bits) - 1;
        for (int src = 0, dst = 0; src < opt->width; src++, dst += opt->palette_bits) {
            int pixel = (rowbuffer[src] & pixmask) << ((8 - opt->palette_bits) - (dst & 7));
            if (dst % 8 == 0)
                rowbuffer[dst / 8] = (uint8_t)pixel;
            else
                rowbuffer[dst / 8] |= (uint8_t)pixel;
        }
    }
    for (int n = 0; n < opt->rowdup; n++) {
        if (opt->current_row >= opt->height)
            return report_error(E_MANYROWS, "manyrows", NULL);
        if (opt->interlaced)
            memcpy(opt->row_ptrs[opt->current_row], rowbuffer, (size_t)opt->stride);
        else
            png_write_row(opt->png_ptr, rowbuffer);
        opt->current_row++;
    }
    return NULL;
}

/* CompressPNG_Finish: R0 is the tag. When the PNG went to a buffer, on exit
 * R0 -> the buffer and R1 is the PNG's length. When only the size was asked
 * for, R0 is 0 and R1 is the size. When it went to a file, the registers
 * are kept. R0 and R1 are set in this way even when an error is given. */
static os_error *finish(uint32_t *r, struct png_opt *opt)
{
    struct cpngmem *mem = opt->mem;
    os_error *e = NULL;
    if (opt->current_row < opt->height) {
        e = report_error(E_FEWROWS, "fewrows", NULL);
        opt->error_state = 1;
    } else if (setjmp(png_jmpbuf(opt->png_ptr))) {
        e = opt->has_pending ? report_error(E_OPTFAIL2, "optfail2", opt->pending)
                             : report_error(E_OPTFAIL, "optfail", NULL);
        opt->has_pending = 0;
        opt->error_state = 1;
    } else {
        if (opt->interlaced)
            png_write_image(opt->png_ptr, opt->row_ptrs);
        png_write_end(opt->png_ptr, NULL);
    }
    switch (opt->destination) {
    case TO_FILE:
        if (opt->handle) {
            if (opt->error_state)
                fclose_and_remove(opt);
            else
                e = fclose_and_settype(opt);
        }
        break;
    case TO_SIZE:
        r[0] = 0;
        r[1] = opt->write_offset;
        break;
    default:
        r[0] = opt->dest_buffer;
        r[1] = opt->write_offset;
        break;
    }
    png_free_data(opt->png_ptr, opt->info_ptr, PNG_FREE_ALL, -1);
    png_destroy_write_struct(&opt->png_ptr, &opt->info_ptr);
    if (opt->dest_filename)
        mem_free(mem, ros_ptr(opt->dest_filename));
    if (opt->row_ptrs)
        mem_free(mem, opt->row_ptrs);
    if (opt->full_source)
        mem_free(mem, opt->full_source);
    mem_free(mem, opt->bounce);
    opt->magic = 0;
    mem_free(mem, opt);
    os_error *e2 = mem_free_area(mem);
    return e ? e : e2;
}

static void dispatch(struct ros_cpu *s, uint32_t offset)
{
    uint32_t *r = s->r;
    os_error *e;
    if (offset == 0) {
        e = start(r);
    } else {
        struct png_opt *opt = tag_of(r[0]);
        if (!opt)
            e = report_error(E_BADTAG, "badtag", NULL);
        else if (offset == 1)
            e = comment(r, opt);
        else if (offset == 2)
            e = writeline(r, opt);
        else
            e = finish(r, opt);
    }
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

void ros_thunk_CompressPNG_Start(struct ros_cpu *s) { dispatch(s, 0); }
void ros_thunk_CompressPNG_Comment(struct ros_cpu *s) { dispatch(s, 1); }
void ros_thunk_CompressPNG_WriteLine(struct ros_cpu *s) { dispatch(s, 2); }
void ros_thunk_CompressPNG_Finish(struct ros_cpu *s) { dispatch(s, 3); }

/* The other SWI numbers in the chunk. The original checks the tag first,
 * and R0 is not a tag in any call that a program makes to them. So they
 * give the bad tag error. */
static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return report_error(E_BADTAG, "badtag", NULL);
}

/* ---- the module ------------------------------------------------------------------------------ */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct cp_workspace *w;
    if (xos_module_claim(sizeof *w, (void **)&w))
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    strcpy(w->zlib_swi, "ZLib_Compress");
    strcpy(w->area_name, "CompressPNG");
    uint32_t r[9] = { 0 };
    w->pagesize = swi(XOS_ReadMemMapInfo, r) || r[0] == 0 ? 4096u : r[0];
    ros_st32(m->private_word, ros_addr(w));
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    mem_free_task(0, 1);
    xos_module_free(ws());
    ros_st32(m->private_word, 0);
    return NULL;
}

/* Service_WimpCloseDown: a task's unfinished compressions are freed. */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] == 0x53 && s->r[0] == 0 && compresspng_module.private_word &&
        ros_ld32(compresspng_module.private_word))
        mem_free_task(s->r[2], 0);
}

struct ros_module compresspng_module = {
    .title = "CompressPNG",
    .help = "CompressPNG\t0.07 (08 Jul 2023) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x59E00,
    .swi_thunks = ros_swi_thunks_CompressPNG,
    .swi_names = ros_swi_names_CompressPNG,
    .swi_prefix = "CompressPNG",
};

__attribute__((constructor)) static void count(void)
{
    compresspng_module.swi_count = ros_swi_count_CompressPNG;
}
