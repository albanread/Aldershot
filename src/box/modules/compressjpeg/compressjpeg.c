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

/* compressjpeg.c -- CompressJPEG (Video/Render/JCompMod, 0.08, 20 Oct
 * 2015), as a native module. Its four SWIs work as c/jcompmod makes them.
 * They run over libjpeg-turbo (built by deps/build-imagelibs.sh). RISC OS
 * has release 5 of the IJG's library instead, so the markers match its
 * output but the entropy-coded data can differ a little (README.md).
 *
 * As in c/jcompmod, there is one compression at a time. Its state is the
 * compression object, and it belongs to the module. In c/jcompmod it is a
 * static. Here it is in the module's workspace, in the RMA. The tag is the
 * object's address, which is the same every time. Starting another
 * compression abandons the one before.
 *
 * libjpeg's memory comes from this module (the part that jmemnoal.c
 * provides, below). It comes from the RMA. When CompressJPEG_Start is given
 * a workspace, it comes from that workspace instead, in 8-byte units, and
 * it is never freed. The PRM gives the size a workspace must be: 20000
 * bytes plus 30 for each pixel across (9 for grey). libjpeg-turbo has
 * 64-bit tables and needs some 22K more than release 5 for colour. So a
 * workspace smaller than the PRM's size fails at the start, as it would on
 * RISC OS. What libjpeg-turbo needs beyond a PRM-sized workspace comes from
 * the RMA. libjpeg-turbo's own memory manager uses malloc. It is not in
 * the library that the box links (see deps/build-imagelibs.sh).
 *
 * Every failure that libjpeg reports becomes "Not enough memory"
 * (&8183C4), as c/jcompmod's setjmp handlers make it. The failure leaves
 * the compression abandoned, and the calls after it fail the same way, as
 * they do there.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <jerror.h>
#include <jmemsys.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "compressjpeg.h"

#define E_NOMEM    4u                   /* ErrorNumber_CompressJPEG_NoMemory */
#define E_STROFLO  5u                   /* ErrorNumber_CompressJPEG_CommentTooLong */
#define MAX_COM_LENGTH 65000u

/* The compression (c/jcompmod's static cinfo and jerr), kept in the workspace. */
struct cj_state {
    struct jpeg_compress_struct cinfo;  /* first: the tag is its address */
    struct jpeg_error_mgr err;
    struct jpeg_destination_mgr dest;
    jmp_buf jb;
    int created;                        /* jpeg_create_compress has been called and the object not destroyed */
    uint32_t buffer, buffer_size;
    uint32_t workspace, workspace_size, memory_used;
};

static struct cj_state *state(void)
{
    return ros_ptr(ros_ld32(compressjpeg_module.private_word));
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

static os_error *abort_at(uint32_t addr)
{
    return ros_error(0x80000002u, "Internal error: abort on data transfer at &%08X", addr);
}

/* The global Messages' NoMem and StrOFlo errors, which c/jcompmod looks up. */
static os_error *error(uint32_t which)
{
    return which == E_NOMEM ? ros_error(COMPRESSJPEG_ERRBASE + E_NOMEM, "Not enough memory")
                            : ros_error(COMPRESSJPEG_ERRBASE + E_STROFLO, "String too long");
}

/* ---- the memory manager (jmemnoal's) ------------------------------------------------------- */

/* An object of the module's own has its state as its client data. An object
 * whose client data is &compressjpeg_host_memory is given host memory.
 * SpriteExtend's JPEG decoder uses this, because it holds whole images,
 * which are more than the RMA should hold. Any other libjpeg object in
 * /init (a self-test's decompressor, or CFSI's) is given RMA. */
char compressjpeg_host_memory;

void *jpeg_get_small(j_common_ptr cinfo, size_t size)
{
    if (cinfo->client_data == &compressjpeg_host_memory)
        return malloc(size ? size : 1);
    struct cj_state *st = cinfo->client_data;
    if (!st || st->workspace == 0) {
        void *b;
        if (size == 0 || size > 0x7FFFFFF0u || xos_module_claim((uint32_t)size, &b))
            return NULL;
        return b;
    }
    size = (size + 7) & ~(size_t)7;
    if (size > st->workspace_size - st->memory_used) {
        void *b;                        /* beyond the PRM's size, so from the RMA */
        if (size > 0x7FFFFFF0u || xos_module_claim((uint32_t)size, &b))
            return NULL;
        return b;
    }
    st->memory_used += (uint32_t)size;
    return ros_ptr(st->workspace + st->memory_used - (uint32_t)size);
}

/* Says whether an object is in the caller's workspace. Such objects are
 * never freed. */
static int in_workspace(const struct cj_state *st, const void *object)
{
    if (!st || !st->workspace || !object)
        return 0;
    uint32_t a = ros_addr(object);
    return a >= st->workspace && a - st->workspace < st->workspace_size;
}

void jpeg_free_small(j_common_ptr cinfo, void *object, size_t size)
{
    (void)size;
    if (cinfo->client_data == &compressjpeg_host_memory) {
        free(object);
        return;
    }
    struct cj_state *st = cinfo->client_data;
    if (object && !in_workspace(st, object))
        xos_module_free(object);
}

void *jpeg_get_large(j_common_ptr cinfo, size_t size)
{
    return jpeg_get_small(cinfo, size);
}

void jpeg_free_large(j_common_ptr cinfo, void *object, size_t size)
{
    jpeg_free_small(cinfo, object, size);
}

size_t jpeg_mem_available(j_common_ptr cinfo, size_t min_bytes_needed, size_t max_bytes_needed,
                          size_t already_allocated)
{
    (void)cinfo, (void)min_bytes_needed, (void)already_allocated;
    return max_bytes_needed;
}

void jpeg_open_backing_store(j_common_ptr cinfo, backing_store_ptr info, long total_bytes_needed)
{
    (void)info, (void)total_bytes_needed;
    ERREXIT(cinfo, JERR_NO_BACKING_STORE);
}

long jpeg_mem_init(j_common_ptr cinfo)
{
    (void)cinfo;
    return 0;
}

void jpeg_mem_term(j_common_ptr cinfo)
{
    (void)cinfo;
}

/* ---- the destination: the caller's buffer (jmemdst's) ------------------------------------- */

static void mem_init_destination(j_compress_ptr cinfo)
{
    struct cj_state *st = cinfo->client_data;
    st->dest.next_output_byte = ros_ptr(st->buffer);
    st->dest.free_in_buffer = st->buffer_size;
}

static boolean mem_empty_output_buffer(j_compress_ptr cinfo)
{
    ERREXIT(cinfo, JERR_BUFFER_SIZE);
    return TRUE;
}

static void mem_term_destination(j_compress_ptr cinfo)
{
    (void)cinfo;
}

/* ---- errors --------------------------------------------------------------------------------- */

static void my_error_exit(j_common_ptr cinfo)
{
    struct cj_state *st = cinfo->client_data;

    longjmp(st->jb, 1);
}

static void my_output_message(j_common_ptr cinfo)
{
    (void)cinfo;                        /* c/jcompmod's warning: "don't output_message from module!" */
}

/* After an error the compression object is destroyed, as c/jcompmod destroys it. */
static os_error *failed(struct cj_state *st)
{
    if (st->created) {
        st->created = 0;
        jpeg_destroy_compress(&st->cinfo);
    }
    return error(E_NOMEM);
}

/* ---- the SWIs -------------------------------------------------------------------------------- */

/* The offsets in the parameter block (jpeg_var_type). */
#define P_WIDTH   0u
#define P_HEIGHT  4u
#define P_QUALITY 8u
#define P_COMPS   12u
#define P_XDPI    16u
#define P_YDPI    20u

/* CompressJPEG_Start: R0 -> the buffer, R1 is its size, R2 -> the
 * parameters, R3 -> the workspace (or 0 to use the RMA) and R4 is its size.
 * On exit R0 is the tag. */
static os_error *start(uint32_t *r)
{
    struct cj_state *st = state();
    if (!valid(r[2], 24))
        return abort_at(r[2]);
    if (!valid(r[0], r[1]))
        return abort_at(r[0]);
    if (r[3] && !valid(r[3], r[4]))
        return abort_at(r[3]);
    /* Fail if the workspace is smaller than the PRM says it must be. */
    uint32_t across = (ros_ld32(r[2] + P_WIDTH) + 15) & ~15u;
    if (r[3] && r[4] < 20000 + across * (ros_ld32(r[2] + P_COMPS) == 1 ? 9u : 30u))
        return error(E_NOMEM);
    if (st->created) {                  /* abandon the last compression */
        st->created = 0;
        jpeg_destroy_compress(&st->cinfo);
    }
    int32_t width = (int32_t)ros_ld32(r[2] + P_WIDTH), height = (int32_t)ros_ld32(r[2] + P_HEIGHT);
    int32_t quality = (int32_t)ros_ld32(r[2] + P_QUALITY), comps = (int32_t)ros_ld32(r[2] + P_COMPS);
    int32_t xdpi = (int32_t)ros_ld32(r[2] + P_XDPI), ydpi = (int32_t)ros_ld32(r[2] + P_YDPI);
    memset(&st->cinfo, 0, sizeof st->cinfo);
    st->buffer = r[0];
    st->buffer_size = r[1];
    st->workspace = r[3];
    st->workspace_size = r[4];
    st->memory_used = 0;
    st->cinfo.err = jpeg_std_error(&st->err);
    st->err.error_exit = my_error_exit;
    st->err.output_message = my_output_message;
    st->cinfo.client_data = st;
    if (setjmp(st->jb))
        return failed(st);
    jpeg_create_compress(&st->cinfo);
    st->cinfo.client_data = st;         /* jpeg_create_compress clears the object except err */
    st->created = 1;
    st->dest.init_destination = mem_init_destination;
    st->dest.empty_output_buffer = mem_empty_output_buffer;
    st->dest.term_destination = mem_term_destination;
    st->cinfo.dest = &st->dest;
    st->cinfo.image_width = (JDIMENSION)width;
    st->cinfo.image_height = (JDIMENSION)height;
    st->cinfo.input_components = comps;
    st->cinfo.in_color_space = comps == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&st->cinfo);
    if (xdpi != 0) {
        st->cinfo.X_density = (UINT16)xdpi;
        st->cinfo.Y_density = (UINT16)ydpi;
        st->cinfo.density_unit = 1;
    } else {
        st->cinfo.X_density = 1;
        st->cinfo.Y_density = 1;
        st->cinfo.density_unit = 0;
    }
    jpeg_set_quality(&st->cinfo, quality, TRUE);
    jpeg_start_compress(&st->cinfo, TRUE);
    r[0] = ros_addr(&st->cinfo);
    return NULL;
}

/* Returns the state if the tag is this module's compression and it is under way. */
static struct cj_state *live(uint32_t tag)
{
    struct cj_state *st = state();
    return tag == ros_addr(&st->cinfo) && st->created ? st : NULL;
}

/* CompressJPEG_WriteLine: R0 is the tag and R1 -> a row (RGB triples, or
 * grey bytes). The registers are kept. */
static os_error *writeline(uint32_t *r)
{
    struct cj_state *st = live(r[0]);
    if (!st)
        return error(E_NOMEM);
    uint32_t bytes = st->cinfo.image_width * (uint32_t)st->cinfo.input_components;
    if (!valid(r[1], bytes))
        return abort_at(r[1]);
    if (setjmp(st->jb))
        return failed(st);
    JSAMPROW row = ros_ptr(r[1]);
    jpeg_write_scanlines(&st->cinfo, &row, 1);
    return NULL;
}

/* CompressJPEG_Finish: R0 is the tag. On exit R0 is the JPEG's length. */
static os_error *finish(uint32_t *r)
{
    struct cj_state *st = live(r[0]);
    if (!st)
        return error(E_NOMEM);
    if (setjmp(st->jb))
        return failed(st);
    jpeg_finish_compress(&st->cinfo);
    r[0] = (uint32_t)(st->buffer_size - st->dest.free_in_buffer);
    st->created = 0;
    jpeg_destroy_compress(&st->cinfo);
    return NULL;
}

/* CompressJPEG_Comment: R0 is the tag. If R1 bit 0 is set, R2 -> a string
 * ended by a control character (it may contain tabs and newlines). If the
 * bit is clear, R2 -> data and R3 is its length. This writes a COM marker,
 * which must come before the first row. */
static os_error *comment(uint32_t *r)
{
    uint32_t length;
    if (r[1] & 1) {
        uint32_t end = r[2];
        for (;;) {
            if (!valid(end, 1))
                return abort_at(end);
            uint32_t c = ros_ld8(end);
            if (!(c >= ' ' || c == 10 || c == 9))
                break;
            end++;
        }
        length = end - r[2];
    } else {
        length = r[3];
    }
    if (length >= MAX_COM_LENGTH)
        return error(E_STROFLO);
    struct cj_state *st = live(r[0]);
    if (!st)
        return error(E_NOMEM);
    if (!valid(r[2], length))
        return abort_at(r[2]);
    if (setjmp(st->jb))
        return failed(st);
    jpeg_write_marker(&st->cinfo, JPEG_COM, ros_ptr(r[2]), length);
    return NULL;
}

static void dispatch(struct ros_cpu *s, uint32_t offset)
{
    uint32_t *r = s->r;
    os_error *e;
    switch (offset) {
    case 0: e = start(r); break;
    case 1: e = writeline(r); break;
    case 2: e = finish(r); break;
    default: e = comment(r); break;
    }
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

void ros_thunk_CompressJPEG_Start(struct ros_cpu *s) { dispatch(s, 0); }
void ros_thunk_CompressJPEG_WriteLine(struct ros_cpu *s) { dispatch(s, 1); }
void ros_thunk_CompressJPEG_Finish(struct ros_cpu *s) { dispatch(s, 2); }
void ros_thunk_CompressJPEG_Comment(struct ros_cpu *s) { dispatch(s, 3); }

/* The error for a bad SWI number, as CMHG's error_BAD_SWI gives it. */
static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module CompressJPEG");
}

/* ---- the module -------------------------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct cj_state *st;
    if (xos_module_claim(sizeof *st, (void **)&st))
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(st, 0, sizeof *st);
    ros_st32(m->private_word, ros_addr(st));
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct cj_state *st = state();
    if (st->created)
        jpeg_destroy_compress(&st->cinfo);
    xos_module_free(st);
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module compressjpeg_module = {
    .title = "CompressJPEG",
    .help = "CompressJPEG\t0.08 (20 Oct 2015) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x4A500,
    .swi_thunks = ros_swi_thunks_CompressJPEG,
    .swi_names = ros_swi_names_CompressJPEG,
    .swi_prefix = "CompressJPEG",
};

__attribute__((constructor)) static void count(void)
{
    compressjpeg_module.swi_count = ros_swi_count_CompressJPEG;
}
