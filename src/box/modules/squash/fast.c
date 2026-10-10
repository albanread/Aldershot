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

/* fast.c -- Squash's fast paths, which RISC OS writes in ARM code.
 * s/comp_ass compresses all of the input at once. s/zcat_ass, as
 * s/zcat_ass12 assembles it, decompresses all of the input at once. Both
 * are 12-bit LZW in compress's format, as the restartable C (c/cssr and
 * c/zssr) is. But each has ways of its own that show in what it writes.
 *
 * - comp_ass clears its table the moment it fills (when code 4095 is
 *   made). cssr keeps using a full table until 32K of input has gone since
 *   the last clear. So the two write different bytes for the same input
 *   once the table has filled. Both decompress to the same data.
 * - After a clear, both skip to where compress's decoder reads its next
 *   chunk of 12 bytes. comp_ass and zcat_ass work the gap out from the
 *   number of bits since the last clear, using their tables of offsets.
 * - comp_ass writes a word at a time, ORing each code into the word it
 *   loads. Its header is three bytes and a zero. If the output is 2 or 3
 *   bytes past a word boundary, the rest of that word is loaded as the
 *   caller left it. So those old bytes (two, or one) are ORed into the
 *   first codes. That is kept here. Its stores also run up to 7 bytes past
 *   the end of what it writes. Those are left alone here.
 * - zcat_ass does not look at the header. It takes the byte after the
 *   header as the first byte out (the first code's low byte), even when
 *   the input ends before it. It stops when fewer than 9 bits are left,
 *   whatever the code size. It writes where it assumes there is room. It
 *   follows a code it does not know yet to wherever its table points. Here
 *   it stops at the end of the room it has, and at a code it does not know
 *   (see sq.h).
 */
#include <stdint.h>

#include "sq.h"

/* The realignment after a clear, for 12-bit codes. The number of bits since
 * the last clear, shifted right by 2 and masked with 7, indexes this table.
 * It is comp_ass's and zcat_ass's row for 12 bits. */
static const uint8_t clear_gap[8] = { 0, 60, 24, 84, 48, 12, 72, 36 };

/* ---- compressing --------------------------------------------------------- */

/* Writes codes at rising bit positions, low bits first. Bytes it has not
 * reached are zeroed as it reaches them. Bytes it skips over are zeroed
 * too. */
struct writer {
    uint8_t *out;
    uint32_t pos;       /* in bits, from out */
    uint32_t fresh;     /* bytes from here on have not been written */
};

static void put(struct writer *w, uint32_t code, uint32_t bits)
{
    uint32_t last = (w->pos + bits - 1) >> 3;
    while (w->fresh <= last)
        w->out[w->fresh++] = 0;
    uint32_t v = code << (w->pos & 7);
    for (uint32_t b = w->pos >> 3; b <= last; b++, v >>= 8)
        w->out[b] |= (uint8_t)v;
    w->pos += bits;
}

uint32_t sq_compress_fast(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t *table)
{
    struct writer w = { out, 24, 4 };
    out[0] = 0x1F;
    out[1] = 0x9D;
    out[2] = 0x80 | SQ_BITS;
    out[3] = 0;
    /* The word the first codes are ORed into, as the caller left it. */
    uintptr_t align = (uintptr_t)out & 3;
    if (align == 2)
        w.fresh = 6;
    else if (align == 3)
        w.fresh = 5;

    uint32_t since = w.pos;             /* where the last clear left off */
    uint32_t previous = *in++, bits;
    n--;
    for (;;) {
        for (int i = 0; i < SQ_HASH_RETRY; i++)
            table[i] = 0xFFFFF000u;     /* empty: code bits clear, the rest set */
        bits = 9;
        uint32_t free_entry = SQ_FIRST;
        for (;;) {
            if (n == 0)
                goto finished;
            n--;
            uint32_t current = *in++;
            uint32_t fcode = previous | current << SQ_BITS;
            int32_t hash = (int32_t)(previous ^ current << (SQ_BITS - 8));
            uint32_t e = table[hash];
            if (e >> SQ_BITS == fcode) {
                previous = e & 0xFFF;
                continue;
            }
            if (e < 0xFFFFF000u) {
                int32_t step = hash ? SQ_HASH_RETRY - hash : 1;
                for (;;) {
                    hash -= step;
                    if (hash < 0)
                        hash += SQ_HASH_RETRY;
                    e = table[hash];
                    if (e >> SQ_BITS == fcode || e >= 0xFFFFF000u)
                        break;
                }
                if (e >> SQ_BITS == fcode) {
                    previous = e & 0xFFF;
                    continue;
                }
            }
            put(&w, previous, bits);
            if (free_entry >= 1u << bits && bits < SQ_BITS)
                bits++;
            previous = current;
            table[hash] = fcode << SQ_BITS | free_entry;
            if (++free_entry >= 1u << SQ_BITS)
                break;
        }
        /* The table is full. Clear it, and skip to the decoder's next chunk. */
        put(&w, SQ_CLEAR, bits);
        w.pos += clear_gap[(w.pos - since) >> 2 & 7];
        since = w.pos;
    }
finished:
    put(&w, previous, bits);
    return (w.pos + 7) >> 3;
}

/* ---- decompressing ------------------------------------------------------- */

/* Reads bits bits at bit position pos of in, low bits first. Past the end
 * of the input it reads zeros. */
static uint32_t get(const uint8_t *in, uint32_t n, uint32_t pos, uint32_t bits)
{
    uint32_t v = 0, b = pos >> 3;
    for (uint32_t i = 0; i < 3; i++)
        if (b + i < n)
            v |= (uint32_t)in[b + i] << 8 * i;
    return v >> (pos & 7) & ((1u << bits) - 1);
}

int sq_decompress_fast(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t room,
                       uint32_t *pointers, uint32_t *used)
{
    uint32_t o = 0;
    int result = 0;
    *used = 0;
    if (room == 0)
        return 1;
    /* The first code is a byte, taken as it is. It is the byte after the
     * header, even when there is none. Then it reads codes until fewer than
     * 9 bits are left. (If that point would be below the header, zcat_ass
     * reads on without end.) A string's bytes are found again in the
     * output. pointers[c] is where code c's string began. Code c's own
     * string runs from pointers[c - 1] to there. */
    int64_t limit = (int64_t)n * 8 - 9;
    uint32_t pos = 24 + 9, since = 24;
    uint32_t free_entry = SQ_FIRST, bits = 9, maxcode = 511, mask = 511;
    pointers[SQ_FIRST - 1] = 0;
    out[o++] = in[3];
    for (;;) {
        if ((int64_t)pos > limit)
            break;
        if (free_entry > maxcode) {
            bits++;
            mask = maxcode * 2 + 1;
            maxcode = mask + (bits >= SQ_BITS);
        }
        pointers[free_entry] = o;
        uint32_t code = get(in, n, pos, bits) & mask;
        pos += bits;
        if (code == SQ_CLEAR) {
            pos += clear_gap[(pos - since) >> 2 & 7];
            since = pos;
            free_entry = SQ_FIRST - 1;
            bits = 9;
            maxcode = mask = 511;
            continue;
        }
        if (code < SQ_CLEAR) {
            if (o >= room) {
                result = 1;
                break;
            }
            out[o++] = (uint8_t)code;
        } else {
            if (code > free_entry) {
                result = -1;
                break;
            }
            uint32_t s = pointers[code - 1];
            uint32_t e = code == free_entry ? o : pointers[code];
            /* Copy two bytes at least, then on to e inclusive. */
            if (o >= room) {
                result = 1;
                break;
            }
            out[o++] = out[s++];
            do {
                if (o >= room) {
                    result = 1;
                    goto stop;
                }
                out[o++] = out[s++];
            } while (s <= e);
        }
        if (free_entry < 1u << SQ_BITS)
            free_entry++;
    }
stop:
    *used = o;
    return result;
}
