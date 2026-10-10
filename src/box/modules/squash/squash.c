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

/* squash.c -- Squash (Programmer/Squash, 0.31), reimplemented. This file
 * has the module, its two SWIs and the calls for native code (squash.h).
 * The SWIs are c/compress's veneer, register for register.
 * README.md lists the flags, the errors and the differences from RISC OS 5.30.
 *
 * The restartable algorithms are RISC OS's own C, c/cssr and c/zssr. They
 * are the objects that Squash's Makefile also builds as SquashLib. They are
 * compiled from its sources as they are (the Makefile copies them to
 * build/gen/squash). Where each call stops, what it has used and what it
 * says are theirs.
 *
 * Their file statics are scratch, loaded from the workspace at each call.
 * The exception is zssr's buf_size. A continued call takes it from the last
 * call made, because zssr stores it into the workspace where it means to
 * load it. That gives the same result unless two decompressions are
 * interleaved, as in RISC OS.
 *
 * The fast algorithms are ARM code in RISC OS and C here (fast.c).
 */
#include <stdint.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "squash/cssr.h"
#include "squash/zssr.h"
#include "sq.h"
#include "squash.h"

/* hdr/NewErrors: CompressErrors, &920. The texts are those of the UK
 * Messages file, COM1 to COM4. */
#define ERR_BAD_ADDRESS    1
#define ERR_BAD_INPUT      2
#define ERR_BAD_WORKSPACE  3
#define ERR_BAD_PARAMETERS 4

static os_error *error(int n)
{
    static const char *const what[] = { "", "address", "input", "workspace", "parameters" };
    return ros_error(0x920u + (uint32_t)n, "Bad %s for module Squash", what[n]);
}

uint32_t squash_compress_max(uint32_t n)
{
    return 12u + 3u * n / 2u;
}

/* c/compress's test for the fast algorithm: "from scratch to end and enough
 * output space worst case". It works in ints. */
static int compress_is_fast(uint32_t flags, uint32_t in_left, uint32_t out_left)
{
    /* This is worked out in 64 bits. In ints, as c/compress has it, three
     * times a large input wraps negative. The test then passes for any
     * output size, and the fast algorithm writes past a small buffer. The
     * two agree for every input under 715 million bytes. */
    int64_t need = 3 + (int64_t)in_left * 3 / 2;
    return !(flags & (SQUASH_CONTINUE | SQUASH_MORE)) && need <= (int32_t)out_left;
}

os_error *squash_compress(uint32_t flags, void *workspace, struct squash_io *io,
                          uint32_t *status)
{
    if (flags >= 0x10u || (flags & SQUASH_SIZES))
        return error(ERR_BAD_PARAMETERS);
    if (compress_is_fast(flags, io->in_left, io->out_left)) {
        /* With nothing in, RISC OS's comp_ass takes the length as 2^32 - 1.
         * Here that case writes nothing. */
        uint32_t used = io->in_left ? sq_compress_fast(io->in, io->in_left, io->out, workspace) : 0;
        io->in += io->in_left;
        io->in_left = 0;
        io->out += used;
        io->out_left -= used;
        *status = SQUASH_DONE;
        return NULL;
    }
    compress_state *state = workspace;
    if (!(flags & SQUASH_CONTINUE))
        state->starting = -1;           /* the field is signed here, so -1 sets its one bit */
    unsigned char *in = (unsigned char *)io->in;
    unsigned int used = io->out_left;
    output_result r = compress_store_store(&in, io->in_left, (char *)io->out, &used, state,
                                           (int)(flags & SQUASH_MORE));
    if (r == output_ws_corrupt)
        return error(ERR_BAD_WORKSPACE);
    io->in_left -= (uint32_t)(in - io->in);
    io->in = in;
    io->out += used;
    io->out_left -= used;
    *status = (uint32_t)r;
    return NULL;
}

os_error *squash_decompress(uint32_t flags, void *workspace, struct squash_io *io,
                            uint32_t *status)
{
    if (flags >= 0x10u || (flags & SQUASH_SIZES))
        return error(ERR_BAD_PARAMETERS);
    if (flags & SQUASH_FAST) {
        /* Bits 0 and 1 are not looked at. All of the input is done at once. */
        uint32_t used;
        int r = sq_decompress_fast(io->in, io->in_left, io->out, io->out_left, workspace, &used);
        if (r < 0)
            return error(ERR_BAD_INPUT);
        io->in += io->in_left;
        io->in_left = 0;
        io->out += used;
        io->out_left -= used;
        *status = r ? SQUASH_NEEDS_ROOM : SQUASH_DONE;
        return NULL;
    }
    zcat_state *state = workspace;
    if (!(flags & SQUASH_CONTINUE))
        state->starting = -1;
    unsigned int in_left = io->in_left;
    zcat_result result;
    unsigned int used = zcat_store_store((unsigned char *)io->in, (char *)io->out, &in_left,
                                         io->out_left, (int)(flags & SQUASH_MORE), state, &result);
    if (result == zcat_input_corrupt)
        return error(ERR_BAD_INPUT);
    if (result == zcat_ws_corrupt)
        return error(ERR_BAD_WORKSPACE);
    io->in += io->in_left - in_left;
    io->in_left = in_left;
    io->out += used;
    io->out_left -= used;
    *status = (uint32_t)result;
    return NULL;
}

/* ---- the SWIs ---------------------------------------------------------------
 *
 * c/compress makes these checks, in this order. First R0 bit 3 (the sizes,
 * on its own). Then the workspace's address and the output's range, by
 * OS_ValidateAddress. Then that R0 is under 16. RISC OS goes on to read the
 * input and use the workspace unchecked, and a bad address aborts there.
 * Here those are checked too and give "Bad address", so that native code
 * does not fault.
 */

static int bad(uint32_t start, uint32_t length)
{
    int invalid = 1;
    xos_validate_address(start, start + length, &invalid);
    return invalid;
}

/* The arguments workspace and input are the numbers of bytes the algorithm
 * will use of each. */
static os_error *checks(const uint32_t *r, uint32_t workspace, uint32_t input)
{
    if (bad(r[1], 0) || bad(r[4], r[5]))
        return error(ERR_BAD_ADDRESS);
    if (r[0] >= 0x10u)
        return error(ERR_BAD_PARAMETERS);
    if (bad(r[1], workspace) || (input && bad(r[2], input)))
        return error(ERR_BAD_ADDRESS);
    return NULL;
}

/* Sets the registers from what an operation did. */
static void done(struct ros_cpu *s, const struct squash_io *io, uint32_t status)
{
    s->r[0] = status;
    s->r[2] += s->r[3] - io->in_left;
    s->r[3] = io->in_left;
    s->r[4] += (uint32_t)(io->out - (uint8_t *)ros_ptr(s->r[4]));
    s->r[5] = io->out_left;
    s->v = 0;
}

/* R0 is the flags. With bit 3 alone, R1 is the input's size or -1. On exit
 * R0 is the workspace size and R1 is the most output (-1 is kept as -1).
 * Otherwise R1 -> workspace, R2 -> input, R3 is its size, R4 -> output and
 * R5 is its size. On exit R0 is the status, R2 to R5 are moved past what
 * was used, and R1 is kept. */
void ros_thunk_Squash_Compress(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    if (r[0] & SQUASH_SIZES) {
        if (r[0] != SQUASH_SIZES) {
            ros_swi_fail(s, error(ERR_BAD_PARAMETERS));
            return;
        }
        r[0] = SQUASH_COMPRESS_WORKSPACE;
        if (r[1] != 0xFFFFFFFFu)
            r[1] = squash_compress_max(r[1]);
        s->v = 0;
        return;
    }
    uint32_t workspace = compress_is_fast(r[0], r[3], r[5]) ? SQ_HASH_RETRY * 4
                                                            : (uint32_t)sizeof(compress_state);
    os_error *e = checks(r, workspace, r[3]);
    struct squash_io io = { ros_ptr(r[2]), r[3], ros_ptr(r[4]), r[5] };
    uint32_t status = 0;
    if (!e)
        e = squash_compress(r[0], ros_ptr(r[1]), &io, &status);
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    done(s, &io, status);
}

/* As Squash_Compress, except that with bit 3 the R1 returned is always -1. */
void ros_thunk_Squash_Decompress(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    if (r[0] & SQUASH_SIZES) {
        if (r[0] != SQUASH_SIZES) {
            ros_swi_fail(s, error(ERR_BAD_PARAMETERS));
            return;
        }
        r[0] = SQUASH_DECOMPRESS_WORKSPACE;
        r[1] = 0xFFFFFFFFu;
        s->v = 0;
        return;
    }
    uint32_t workspace = (r[0] & SQUASH_FAST) ? ((1u << SQ_BITS) + 1) * 4
                                              : (uint32_t)sizeof(zcat_state);
    /* The fast algorithm takes the byte after the header, R2 + 3, however
     * little input there is. */
    uint32_t input = (r[0] & SQUASH_FAST) && r[3] < 4 ? 4 : r[3];
    os_error *e = checks(r, workspace, input);
    struct squash_io io = { ros_ptr(r[2]), r[3], ros_ptr(r[4]), r[5] };
    uint32_t status = 0;
    if (!e)
        e = squash_decompress(r[0], ros_ptr(r[1]), &io, &status);
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    done(s, &io, status);
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6, "SWI value out of range for module Squash");
}

struct ros_module squash_module = {
    .title = "Squash",
    .help = "Squash\t\t0.31 (11 Feb 2023) ROSGD native",
    .bad_swi = bad_swi,
    .swi_chunk = 0x42700,
    .swi_thunks = ros_swi_thunks_Squash,
    .swi_names = ros_swi_names_Squash,
    .swi_prefix = "Squash",
};

__attribute__((constructor)) static void count(void)
{
    squash_module.swi_count = ros_swi_count_Squash;
}
