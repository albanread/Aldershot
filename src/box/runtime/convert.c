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
 * This file is a reimplementation in C of RISC OS Open's kernel source
 * (Sources/Kernel: s.Convrsions, s.ArthurSWIs).
 */

/* convert.c: the kernel's number conversions, reimplemented.
 *
 * Every OS_Convert SWI is one engine in the kernel (Kernel/s/Convrsions).
 * The engine is OS_ConvertVariform, which formats a value of a given number
 * of bytes as a given type into a buffer. OS_ConvertHex1 to
 * OS_ConvertSpacedInteger4 are that engine with a size and a type from a
 * table. The file sizes and net stations are the engine with padding rules.
 * This file is the same engine in C, written from that source, with the
 * SWIs over it. It also holds OS_BinaryToDecimal and OS_ReadUnsigned
 * (Kernel/s/ArthurSWIs).
 *
 * What callers depend on is kept exactly.
 *
 *   - On exit R0 is the buffer, R1 the terminator and R2 the bytes left. The
 *     terminator needs a byte but does not use one up. A full buffer is an
 *     overflow, "Buffer overflow" &1E4.
 *   - OS_ReadUnsigned's errors give R2 = 0. R1 is unmoved unless the number
 *     was read. A bad terminator or an out-of-range number leaves R1 after
 *     the number, as the kernel's does. The texts are RISC OS 5.30's:
 *     "Number not recognised", "Number too big" and "Base not recognised".
 *     The text is "(Number)" for no digits or too many digits, because the
 *     kernel translates it twice.
 *   - OS_ConvertVariform with R2 negative counts. It answers with the
 *     overflow error and R2 = NOT the length needed, as the kernel does.
 *   - The formats are right to the character. These are leading-zero
 *     suppression, the spaced and punctuated groupings, file sizes rounded
 *     to four digits and a unit, and Econet's padded "nnn.sss". IPv4 and
 *     EUI-48 are read from the top byte down. IPv6 has its longest run of
 *     zero groups shortened.
 *   - The punctuated forms ask the territory for its thousands separator
 *     (Territory_ReadSymbols 1) and use its first character.
 *
 * OS_ConvertDateAndTime and OS_ConvertStandardDateAndTime belong to the
 * territory, and the kernel passes them on. They go to
 * Territory_ConvertDateAndTime and Territory_ConvertStandardDateAndTime for
 * the configured territory (modules/territory). The formats are the
 * territory's. Local time is made from the CMOS's time zone and daylight
 * saving settings.
 */
#include <string.h>

#include <stdio.h>
#include <stdlib.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

enum {
    TO_HEX, TO_CARDINAL, TO_INTEGER, TO_BINARY, TO_SPACED_CARDINAL, TO_SPACED_INTEGER,
    TO_PUNCT_CARDINAL, TO_PUNCT_INTEGER, TO_FIXED_FILE_SIZE, TO_FILE_SIZE, TO_IPV4, TO_EUI,
    TO_IPV6, TO_SHORTEST_IPV6, TO_HEX_LOWERCASE, TO_UUID, TO_TYPES
};

#define ERR_BUFF_OVERFLOW 0x1E4u
#define ERR_UN_CONV       0x1F1u
#define ERR_BAD_NUMB      0x16Bu
#define ERR_NUMB_TOO_BIG  0x16Cu
#define ERR_BAD_BASE      0x16Au
#define ERR_BAD_NETWORK   0x307u
#define ERR_BAD_STATION   0x306u

/* ---- output ---- */

struct out {
    uint32_t p;                 /* next byte, an arena address, or an index in host */
    uint32_t left;              /* bytes left */
    os_error *e;
    uint8_t *host;              /* not NULL: counting, into this */
};

static os_error *overflow(void)
{
    return ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow");
}

/* One character. 0 is the terminator, which is written but not counted. */
static int put(struct out *o, uint32_t c)
{
    if (o->e)
        return -1;
    if (o->left == 0) {
        o->e = overflow();
        return -1;
    }
    if (o->host)
        o->host[o->p] = (uint8_t)c;
    else
        ros_st8(o->p, c);
    if (c) {
        o->p++;
        o->left--;
    }
    return 0;
}

static int fail(struct out *o, uint32_t errnum, const char *text)
{
    if (!o->e)
        o->e = ros_error(errnum, "%s", text);
    return -1;
}

static int output(struct out *o, const uint8_t *in, uint32_t n, uint32_t type);

/* ---- the forms ---- */

static int binhex(struct out *o, const uint8_t *in, uint32_t n, uint32_t type)
{
    if (type == TO_BINARY) {
        for (uint32_t bit = n * 8; bit-- > 0;)
            if (put(o, '0' + ((in[bit / 8] >> (bit % 8)) & 1)))
                return -1;
    } else {                    /* n nybbles, the top one first */
        for (uint32_t k = n; k-- > 0;) {
            uint32_t v = (in[k / 2] >> (4 * (k % 2))) & 15;
            uint32_t c = v < 10 ? '0' + v : 'A' + v - 10;
            if (type == TO_HEX_LOWERCASE)
                c |= 0x20;
            if (put(o, c))
                return -1;
        }
    }
    return put(o, 0);
}

static int decimal(struct out *o, const uint8_t *in, uint32_t n, uint32_t type)
{
    if (n > 8)
        return fail(o, ERR_BAD_NUMB, "Bad number");
    uint64_t v = 0;
    for (uint32_t i = n; i-- > 0;)
        v = v << 8 | in[i];
    uint32_t sep = type == TO_SPACED_CARDINAL || type == TO_SPACED_INTEGER ? ' ' : 0;
    if (type == TO_PUNCT_CARDINAL || type == TO_PUNCT_INTEGER) {
        /* The territory's thousands separator, using its first character
         * (none if it is ""), as the kernel's VariformOutputDecPunct reads
         * it. */
        uint32_t symbol;
        os_error *e = xterritory_read_symbols(-1, 1, &symbol);
        if (e) {
            if (!o->e)
                o->e = e;
            return -1;
        }
        sep = ros_ld8(symbol);
    }
    if (type == TO_INTEGER || type == TO_SPACED_INTEGER || type == TO_PUNCT_INTEGER) {
        if (n < 8 && (v >> (n * 8 - 1)) & 1)
            v |= ~0ull << (n * 8);          /* sign-extend */
        if ((int64_t)v < 0) {
            if (put(o, '-'))
                return -1;
            v = 0 - v;
        }
    }
    char digits[24];
    int nd = 0;
    do
        digits[nd++] = (char)('0' + v % 10);
    while ((v /= 10) != 0);
    for (int i = nd - 1; i >= 0; i--) {
        if (put(o, (uint8_t)digits[i]))
            return -1;
        /* A separator after the digit for 10^3, 10^6 ... */
        if (sep && i > 0 && i % 3 == 0 && put(o, sep))
            return -1;
    }
    return put(o, 0);
}

static int file_size(struct out *o, const uint8_t *in, uint32_t n, uint32_t type)
{
    if (n > 8)
        return fail(o, ERR_BAD_NUMB, "Bad number");
    uint64_t v = 0;
    for (uint32_t i = n; i-- > 0;)
        v = v << 8 | in[i];
    unsigned unit = 0;
    while (v >= 4096) {
        v = (v >> 10) + ((v >> 9) & 1);     /* divided by 1024, rounding the lost bit */
        unit++;
    }
    if (type == TO_FIXED_FILE_SIZE)
        for (uint64_t w = 1000; w > 1; w /= 10)
            if (v < w && put(o, ' '))
                return -1;
    uint8_t two[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    if (decimal(o, two, 2, TO_CARDINAL) || put(o, ' '))
        return -1;
    uint32_t prefix = (uint8_t)" kMGTPE"[unit];
    if (type == TO_FILE_SIZE && prefix == ' ')
        prefix = 0;                         /* no padding unit */
    if (put(o, prefix))
        return -1;
    for (const char *s = "byte"; *s; s++)
        if (put(o, (uint8_t)*s))
            return -1;
    uint32_t plural = v != 1 ? 's' : type == TO_FILE_SIZE ? 0 : ' ';
    if (put(o, plural))
        return -1;
    return put(o, 0);
}

/* IPv4 as cardinals and EUI as hex pairs, both from the top byte down. */
static int dot_colon(struct out *o, const uint8_t *in, uint32_t n, uint32_t type)
{
    for (uint32_t k = n; k-- > 0;) {
        uint8_t b = in[k];
        if (type == TO_IPV4 ? output(o, &b, 1, TO_CARDINAL) : output(o, &b, 2, TO_HEX))
            return -1;
        if (put(o, k ? (type == TO_IPV4 ? '.' : ':') : 0))
            return -1;
    }
    return 0;
}

/* IPv6: halfwords from the top down, each in the fewest lowercase digits. */
static int ipv6(struct out *o, const uint8_t *in, uint32_t n)
{
    if (n & 1)
        return fail(o, ERR_BAD_NUMB, "Bad number");
    for (uint32_t k = n; k > 0; k -= 2) {
        uint32_t h = (uint32_t)in[k - 1] << 8 | in[k - 2];
        uint32_t digits = 1 + (h >> 4 != 0) + (h >> 8 != 0) + (h >> 12 != 0);
        uint8_t le[2] = { (uint8_t)h, (uint8_t)(h >> 8) };
        if (output(o, le, digits, TO_HEX_LOWERCASE))
            return -1;
        if (put(o, k > 2 ? ':' : 0))
            return -1;
    }
    return 0;
}

/* The kernel's scan for the longest run of zero halfwords. It takes the last
 * of the longest runs, counted in bytes from the start. A run of one is not
 * shortened. */
static int shortest_ipv6(struct out *o, const uint8_t *in, uint32_t n)
{
    if (n & 1)
        return fail(o, ERR_BAD_NUMB, "Bad number");
    uint32_t this_start = 0, this_run = 0, max_start = 0, max_run = 0, state = 2;
    for (uint32_t i = 0; i < n; i += 2) {
        if (in[i] | in[i + 1])
            state |= 1;
        state &= 3;
        if (state == 0)
            this_run++;                     /* in a run */
        else if (state == 2) {
            this_run = 1;                   /* a run starts */
            this_start = i;
        }
        if (this_run >= max_run) {
            max_run = this_run;
            max_start = this_start;
        }
        state <<= 1;
    }
    if (max_run <= 1)
        return ipv6(o, in, n);
    uint32_t end = max_start + max_run * 2;
    if (output(o, in + end, n - end, TO_IPV6) || put(o, ':') || put(o, ':'))
        return -1;
    return output(o, in, max_start, TO_IPV6);
}

static int uuid(struct out *o, const uint8_t *in, uint32_t n)
{
    if (n != 16)
        return fail(o, ERR_BAD_NUMB, "Bad number");
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t b = in[i];
        if (output(o, &b, 2, TO_HEX_LOWERCASE))
            return -1;
        if ((i == 3 || i == 5 || i == 7 || i == 9) && put(o, '-'))
            return -1;
    }
    return put(o, 0);
}

/* The kernel's VariformOutput: n bytes (nybbles, for hex) of in as type. */
static int output(struct out *o, const uint8_t *in, uint32_t n, uint32_t type)
{
    if (n == 0)
        return put(o, 0);                   /* asking for nothing gives a null string */
    switch (type) {
    case TO_HEX: case TO_BINARY: case TO_HEX_LOWERCASE: return binhex(o, in, n, type);
    case TO_CARDINAL: case TO_INTEGER: case TO_SPACED_CARDINAL: case TO_SPACED_INTEGER:
    case TO_PUNCT_CARDINAL: case TO_PUNCT_INTEGER: return decimal(o, in, n, type);
    case TO_FIXED_FILE_SIZE: case TO_FILE_SIZE: return file_size(o, in, n, type);
    case TO_IPV4: case TO_EUI: return dot_colon(o, in, n, type);
    case TO_IPV6: return ipv6(o, in, n);
    case TO_SHORTEST_IPV6: return shortest_ipv6(o, in, n);
    case TO_UUID: return uuid(o, in, n);
    default: return fail(o, ERR_UN_CONV, "Unsupported conversion");
    }
}

/* ---- the SWIs ---- */

/* Finish a conversion into the registers. On success R0 is the buffer, R1
 * the terminator and R2 what is left. On an error, R1 is the buffer. */
static void finish(struct ros_cpu *s, uint32_t buffer, struct out *o)
{
    if (o->e) {
        s->r[1] = buffer;
        s->r[2] = o->left;
        ros_swi_fail(s, o->e);
        return;
    }
    s->r[0] = buffer;
    s->r[1] = o->p;
    s->r[2] = o->left;
    s->v = 0;
}

static void convert(struct ros_cpu *s, const uint8_t *in, uint32_t n, uint32_t type)
{
    uint32_t buffer = s->r[1];
    struct out o = { buffer, s->r[2], NULL, NULL };
    output(&o, in, n, type);
    finish(s, buffer, &o);
}

/* OS_ConvertVariform: R0 points to the value, R3 is its size and R4 the
 * type. If R2 < 0 the SWI counts. It answers with the overflow error and
 * R2 = NOT the length. */
void ros_thunk_OS_ConvertVariform(struct ros_cpu *s)
{
    uint32_t n = s->r[3], type = s->r[4];
    const uint8_t *in = ros_ptr(s->r[0]);
    if ((int32_t)s->r[2] >= 0) {
        convert(s, in, n, type);
        return;
    }
    uint32_t len;
    switch (type) {
    case TO_BINARY: len = n * 8; break;
    case TO_HEX: case TO_HEX_LOWERCASE: len = n * 2; break;   /* sic: per nybble */
    case TO_FIXED_FILE_SIZE: len = 4 + 1 + 1 + 5; break;
    case TO_EUI: len = n * 3 - 1; break;
    case TO_UUID: len = 16 * 2 + 4; break;
    default: {
        /* The length is hard to know, so do the conversion into 44 bytes,
         * the longest (IPv6), and count. */
        uint8_t scratch[44];
        struct out o = { 0, sizeof scratch, NULL, scratch };
        if (output(&o, in, n, type)) {
            ros_swi_fail(s, o.e);
            return;
        }
        len = o.p;
    }
    }
    s->r[2] = ~len;
    ros_swi_fail(s, overflow());
}

/* The table-driven SWIs. Each takes the value in R0 and has a size and a
 * type (the kernel's ConvertSizesTable and ConvertTypesTable). */
#define CONVERT(name, size, type)                                           \
    void ros_thunk_##name(struct ros_cpu *s)                                \
    {                                                                       \
        uint8_t in[4];                                                      \
        memcpy(in, &s->r[0], 4);                                            \
        convert(s, in, size, type);                                         \
    }
CONVERT(OS_ConvertHex1, 1, TO_HEX)
CONVERT(OS_ConvertHex2, 2, TO_HEX)
CONVERT(OS_ConvertHex4, 4, TO_HEX)
CONVERT(OS_ConvertHex6, 6, TO_HEX)
CONVERT(OS_ConvertHex8, 8, TO_HEX)
CONVERT(OS_ConvertCardinal1, 1, TO_CARDINAL)
CONVERT(OS_ConvertCardinal2, 2, TO_CARDINAL)
CONVERT(OS_ConvertCardinal3, 3, TO_CARDINAL)
CONVERT(OS_ConvertCardinal4, 4, TO_CARDINAL)
CONVERT(OS_ConvertInteger1, 1, TO_INTEGER)
CONVERT(OS_ConvertInteger2, 2, TO_INTEGER)
CONVERT(OS_ConvertInteger3, 3, TO_INTEGER)
CONVERT(OS_ConvertInteger4, 4, TO_INTEGER)
CONVERT(OS_ConvertBinary1, 1, TO_BINARY)
CONVERT(OS_ConvertBinary2, 2, TO_BINARY)
CONVERT(OS_ConvertBinary3, 3, TO_BINARY)
CONVERT(OS_ConvertBinary4, 4, TO_BINARY)
CONVERT(OS_ConvertSpacedCardinal1, 1, TO_SPACED_CARDINAL)
CONVERT(OS_ConvertSpacedCardinal2, 2, TO_SPACED_CARDINAL)
CONVERT(OS_ConvertSpacedCardinal3, 3, TO_SPACED_CARDINAL)
CONVERT(OS_ConvertSpacedCardinal4, 4, TO_SPACED_CARDINAL)
CONVERT(OS_ConvertSpacedInteger1, 1, TO_SPACED_INTEGER)
CONVERT(OS_ConvertSpacedInteger2, 2, TO_SPACED_INTEGER)
CONVERT(OS_ConvertSpacedInteger3, 3, TO_SPACED_INTEGER)
CONVERT(OS_ConvertSpacedInteger4, 4, TO_SPACED_INTEGER)
CONVERT(OS_ConvertFixedFileSize, 4, TO_FIXED_FILE_SIZE)
CONVERT(OS_ConvertFileSize, 4, TO_FILE_SIZE)

/* Econet's net.station: R0 points to two words, the station and then the
 * net. The fixed form pads to "nnn.sss". It uses spaces for the net, and
 * zeros for the station after a net. When the net is 0 it uses all spaces.
 * The other form has no padding, and no net when the net is 0. */
static int net_to_dec(struct out *o, uint32_t v, uint32_t pad)
{
    if (v < 100 && put(o, pad))
        return -1;
    if (v < 10 && put(o, pad))
        return -1;
    if (v == 0)
        return put(o, pad);                 /* a pad in place of "0" */
    uint8_t b = (uint8_t)v;
    return decimal(o, &b, 1, TO_CARDINAL);
}

static void net_station(struct ros_cpu *s, int fixed)
{
    uint32_t station = ros_ld32(s->r[0]), net = ros_ld32(s->r[0] + 4);
    if (net >= 256) {
        ros_swi_fail(s, ros_error(ERR_BAD_NETWORK, "Bad network number"));
        return;
    }
    if (station == 0 || station >= 256) {
        ros_swi_fail(s, ros_error(ERR_BAD_STATION, "Bad station number"));
        return;
    }
    uint32_t buffer = s->r[1];
    struct out o = { buffer, s->r[2], NULL, NULL };
    uint32_t pad = fixed ? ' ' : 0;
    if (!net_to_dec(&o, net, pad)) {
        uint32_t dot = '.';
        if (!fixed) {
            if (net == 0)
                dot = 0;
            pad = 0;
        } else {
            if (net == 0)
                dot = ' ';
            pad = net == 0 ? ' ' : '0';
        }
        if (!put(&o, dot))
            net_to_dec(&o, station, pad);
    }
    finish(s, buffer, &o);
}

void ros_thunk_OS_ConvertFixedNetStation(struct ros_cpu *s)
{
    net_station(s, 1);
}

void ros_thunk_OS_ConvertNetStation(struct ros_cpu *s)
{
    net_station(s, 0);
}

/* OS_BinaryToDecimal: a signed word, with no terminator. On exit R2 is the
 * number of characters. */
void ros_thunk_OS_BinaryToDecimal(struct ros_cpu *s)
{
    int64_t v = (int32_t)s->r[0];
    uint32_t buffer = s->r[1], n = 0;
    int32_t room = (int32_t)s->r[2];        /* signed, so a negative size overflows */
    char digits[12];
    int nd = 0;
    uint64_t m = v < 0 ? (uint64_t)-v : (uint64_t)v;
    do
        digits[nd++] = (char)('0' + m % 10);
    while ((m /= 10) != 0);
    if (v < 0) {
        if (room < 1)
            goto full;
        ros_st8(buffer + n++, '-');
        room--;
    }
    while (nd > 0) {
        if (room < 1)
            goto full;
        ros_st8(buffer + n++, (uint8_t)digits[--nd]);
        room--;
    }
    s->r[2] = n;
    s->v = 0;
    return;
full:
    s->r[2] = n;
    ros_swi_fail(s, overflow());
}

/* ---- OS_ReadUnsigned (Kernel/s/ArthurSWIs) ---- */

#define WIDE 0x45444957u            /* "WIDE": R4 for the 64-bit interface */

static int digit(uint8_t c, uint32_t base)
{
    int v = c >= '0' && c <= '9' ? c - '0'
          : c >= 'A' && c <= 'Z' ? c - 'A' + 10
          : c >= 'a' && c <= 'z' ? c - 'a' + 10
          : 99;
    return v < (int)base ? v : -1;
}

/* Read digits in base from *p. On success return 0 with the value in *v,
 * and move *p on. Otherwise return an error number. No digits is BadNumb. Too
 * many for the width is NumbTooBig. */
static uint32_t read_in_base(uint32_t *p, uint32_t base, int wide, uint64_t *v)
{
    uint64_t r = 0, max = wide ? ~0ull : 0xFFFFFFFFull;
    uint32_t a = *p, used = 0;
    for (int d; (d = digit((uint8_t)ros_ld8(a), base)) >= 0; a++, used++) {
        if (r > (max - (uint64_t)d) / base)
            return ERR_NUMB_TOO_BIG;
        r = r * base + (uint64_t)d;
    }
    if (!used)
        return ERR_BAD_NUMB;
    *v = r;
    *p = a;
    return 0;
}

void ros_thunk_OS_ReadUnsigned(struct ros_cpu *s)
{
    uint32_t flags = s->r[0], p = s->r[1];
    int wide_api = s->r[4] == WIDE;
    uint32_t mask = wide_api ? 0xF0000000u : 0xE0000000u;
    uint32_t in = flags & mask;
    uint32_t base = wide_api ? flags & 255 : flags & ~mask;
    int wide = (in >> 28) & 1;
    uint64_t limit = ~0ull;
    if (in & (3u << 29))
        limit = (uint64_t)(wide ? s->r[3] : 0) << 32 | s->r[2];
    if (in & (1u << 30))
        limit = 0xFF;                       /* set after bit 29, so it wins, as the kernel's code has it */
    if ((int32_t)base < 2 || base > 36)
        base = 10;

    uint32_t err = 0, twice = 0;
    uint64_t v = 0;
    while (ros_ld8(p) == ' ')
        p++;
    if (ros_ld8(p) == '&') {
        p++;
        err = read_in_base(&p, 16, wide, &v);
    } else {
        uint32_t q = p, b = base;
        uint64_t d;
        if (read_in_base(&q, 10, 0, &d) == 0 && ros_ld8(q) == '_') {
            p = q + 1;                      /* base_number */
            b = d > 0xFFFFFFFFu ? 0 : (uint32_t)d;
        }
        if (b < 2 || b > 36)
            err = ERR_BAD_BASE;
        else
            err = read_in_base(&p, b, wide, &v);
    }
    twice = err == ERR_BAD_NUMB || err == ERR_NUMB_TOO_BIG;
    if (!err) {
        s->r[1] = p;                        /* R1 is set from here on, for errors too */
        if ((in & (1u << 31)) && ros_ld8(p) > ' ')
            err = ERR_BAD_NUMB;             /* a bad terminator */
        else if (v > limit)
            err = ERR_NUMB_TOO_BIG;
    }
    if (wide_api)
        s->r[4] = mask;                     /* what this kernel supports */
    if (err) {
        s->r[2] = 0;
        if (wide)
            s->r[3] = 0;
        /* The texts are from the kernel's Messages file. An error from
         * reading the digits is translated twice there, first in
         * ReadNumberInBase and then at the SWI's exit. The second time
         * "Number" is the token. */
        ros_swi_fail(s, ros_error(err, "%s", twice ? "(Number)"
                                              : err == ERR_BAD_BASE ? "Base not recognised"
                                              : err == ERR_BAD_NUMB ? "Number not recognised"
                                              : "Number too big"));
        return;
    }
    s->r[2] = (uint32_t)v;
    if (wide)
        s->r[3] = (uint32_t)(v >> 32);
    s->v = 0;
}

/* ---- dates ---- */

/* The kernel's DateTime_Code and StandardDateTime_Code. This calls the
 * territory's conversion with R0 = -1 (the configured territory) and every
 * register moved up by one. A negative size is refused with Buffer overflow.
 * R3 and R4 are kept. */
static void territory_date(struct ros_cpu *s, uint32_t swi_number, uint32_t format)
{
    if ((int32_t)s->r[2] < 0) {
        ros_swi_fail(s, overflow());
        return;
    }
    uint32_t outer = ros_svc_sp_enter(s);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0xFFFFFFFFu, c.r[1] = s->r[0], c.r[2] = s->r[1], c.r[3] = s->r[2];
    c.r[4] = format;
    ros_swi(&c, swi_number | ROS_X_BIT);
    ros_svc_sp = outer;
    s->r[0] = c.r[0], s->r[1] = c.r[1], s->r[2] = c.r[2];
    s->v = c.v;
}

/* OS_ConvertDateAndTime: R0 points to five bytes, R1 is the buffer, R2 the
 * size and R3 the format. */
void ros_thunk_OS_ConvertDateAndTime(struct ros_cpu *s)
{
    territory_date(s, Territory_ConvertDateAndTime, s->r[3]);
}

/* OS_ConvertStandardDateAndTime: the territory's standard format */
void ros_thunk_OS_ConvertStandardDateAndTime(struct ros_cpu *s)
{
    territory_date(s, Territory_ConvertStandardDateAndTime, 0);
}
