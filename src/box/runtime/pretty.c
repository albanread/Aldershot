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
 * This file is a reimplementation in C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.MoreSWIs).
 */
/* pretty.c -- OS_PrettyPrint, reimplemented.
 *
 * This follows the kernel's XOS_PrettyPrint_code (Kernel/s/MoreSWIs).
 * R0 is a NUL-terminated text. It is printed with a line break before any
 * word that would not fit on the current line. CR forces a new line. TAB
 * moves to the next multiple of eight, or to a new line when the word
 * after it would not fit there. 31 is a hard space. 27 n is the nth entry
 * of the dictionary at R1, or of the kernel's dictionary if R1 is 0. For
 * n = 0 it is the string at R2. A dictionary is a series of entries. Each
 * entry is a length byte (the entry's whole length) and a NUL-terminated
 * string. Tokens may nest. Anything else between words is a separator and
 * is dropped.
 *
 * The line width is the window's width. That is VDU variable 256,
 * WindowWidth, as the kernel's ReadWindowWidth reads it: the number of
 * characters that fit after a new line. They are counted from the start of
 * the line. The kernel starts from OS_Byte 165's column, which a task
 * window's output never moves, so its *Help there starts from 0 too.
 *
 * The kernel's dictionary is Resources:$.Resources.Kernel.Dictionary, as
 * MessageTrans looks for it. Without one, its tokens print as nothing.
 */
#include <string.h>

#include "resourcefs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define NOLEAD 0x80000000u          /* the kernel's "no leading space" flag */
#define DEPTH 16

/* The reader: the text, and the tokens it is inside */
struct reader {
    uint32_t p;
    uint32_t stack[DEPTH];
    int sp;
    uint32_t dict, special;
};

static uint32_t entry(const struct reader *r, uint32_t n)
{
    uint32_t e = r->dict;
    if (!e)
        return 0;
    while (--n) {
        uint32_t len = ros_ld8(e);
        if (!len)
            return 0;
        e += len;
    }
    return ros_ld8(e) ? e + 1 : 0;
}

/* The next character, tokens expanded (the kernel's getbytepp) */
static uint32_t next(struct reader *r)
{
    for (;;) {
        uint32_t c = ros_ld8(r->p++);
        if (c == 27) {
            uint32_t n = ros_ld8(r->p++);
            uint32_t to = n == 0 ? r->special : entry(r, n);
            if (to && r->sp < DEPTH) {
                r->stack[r->sp++] = r->p;
                r->p = to;
            }
            continue;
        }
        if (c == 0 && r->sp > 0) {
            r->p = r->stack[--r->sp];
            continue;
        }
        return c;
    }
}

/* The next word's length, without moving on (getwordlength) */
static uint32_t word_length(const struct reader *r)
{
    struct reader copy = *r;
    uint32_t n = 0;
    for (uint32_t c; (c = next(&copy)) == 31 || c > ' ';)
        n++;
    return n;
}

static int out(uint32_t c)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = c;
    ros_swi(&s, XOS_WriteC);
    return s.v ? (int)s.r[0] : 0;
}

static int newline(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, XOS_NewLine);
    return s.v ? (int)s.r[0] : 0;
}

/* The kernel's ReadWindowWidth: 80 columns where the VDU cannot say */
static int32_t window_width(void)
{
    uint32_t *b = ros_rma_alloc(16), w = 80;
    if (!b)
        return (int32_t)w;
    uint32_t in = ros_addr(b), out = in + 8;
    ros_st32(in, 256);                  /* WindowWidth */
    ros_st32(in + 4, (uint32_t)-1);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = in, s.r[1] = out;
    ros_swi(&s, XOS_ReadVduVariables);
    if (!s.v)
        w = ros_ld32(out);
    ros_rma_free(b);
    return (int32_t)w;
}

static uint32_t pretty(struct reader *r, int32_t width)
{
    int32_t pos = (int32_t)NOLEAD;      /* at the start of a line: no leading space */
    uint32_t e, c;
    for (;;) {
        uint32_t len = word_length(r);
        if (len == 0) {
            c = next(r);
        } else {
            if (pos > 0)
                len++;                  /* the separator */
            if ((int32_t)(len + ((uint32_t)pos & ~NOLEAD)) > width) {
                if (pos > 0)
                    len--;
                pos = 0;
                if ((e = (uint32_t)newline()) != 0)
                    return e;
            }
            int lead = pos > 0;
            pos = (int32_t)(((uint32_t)pos & ~NOLEAD) + len);
            if (lead) {
                len--;
                if ((e = (uint32_t)out(' ')) != 0)
                    return e;
            }
            while (len--) {
                c = next(r);
                if ((e = (uint32_t)out(c == 31 ? ' ' : c)) != 0)
                    return e;
            }
        }
    again:
        if (c == 13) {
            pos = 0;
            if ((e = (uint32_t)newline()) != 0)
                return e;
        }
        if (c == 9) {
            pos = (int32_t)((uint32_t)pos & ~NOLEAD);
            int32_t spaces = ((pos + 8) & ~7) - pos;
            uint32_t w;
            for (;;) {
                if ((w = word_length(r)) != 0)
                    break;
                c = next(r);
                if (c == 13)
                    goto again;
                if (c == 9)
                    spaces += 8;
                else
                    spaces--;
                if (c == 0) {
                    r->p--;
                    break;
                }
            }
            if (pos + spaces + (int32_t)w > width) {
                c = 13;
                goto again;
            }
            for (; spaces > 0; spaces--, pos++)
                if ((e = (uint32_t)out(' ')) != 0)
                    return e;
            pos = (int32_t)((uint32_t)pos | NOLEAD);
            continue;
        }
        if (c == 0)
            return 0;
    }
}

void ros_thunk_OS_PrettyPrint(struct ros_cpu *s)
{
    struct reader r = { s->r[0], { 0 }, 0, s->r[1], s->r[2] };
    if (!r.dict) {
        uint32_t f = ros_resourcefs_find("Resources:$.Resources.Kernel.Dictionary");
        r.dict = f ? f + 4 : 0;
    }
    uint32_t outer = ros_svc_sp_enter(s);
    uint32_t e = pretty(&r, window_width());
    ros_svc_sp = outer;
    if (e)
        ros_swi_fail(s, ros_ptr(e));
    else
        s->v = 0;
}
