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
/* readargs.c -- OS_ReadArgs, reimplemented.
 *
 * This follows the kernel's RdArgs (Kernel/s/MoreSWIs). R0 is the keys:
 * "name=alias/q, ..." with the qualifiers /A (always needed), /K (keyword
 * only), /S (a switch), /E (evaluated) and /G (GSTransed). R1 is the
 * command line. R2 and R3 are the output buffer and its size. On exit the
 * buffer starts with a word for each key. The word is 0 for an absent key
 * and a pointer to the value for a key that was given. For a switch it is
 * a non-zero word. R3 is what is left.
 *
 * These are kept as the kernel has them:
 *   - an option is "-name" or its first letter. Switches given by letter
 *     can run together: "-ab" is -a -b;
 *   - items fill the next key that is neither /K nor /S, in order;
 *   - quoted items keep their spaces, and "" inside is a quote;
 *   - /E values are a type byte (0 integer, 1 string) and then four bytes,
 *     or two bytes of length and the characters. /G values are two bytes
 *     of length and the characters;
 *   - the errors are "Parameters not recognised" &1EA, "Argument repeated"
 *     &1EB, "Buffer overflow" &1E4 and "String not recognised" &FD.
 *
 * There is one difference. An item beginning with '-' that names no key is
 * meant to be an ordinary item, "-5" say. The kernel restores its pointer
 * one byte short there and copies from the character before. Here the item
 * is kept whole, '-' and all.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"

#define A_FLAG (1u << 8)
#define K_FLAG (1u << 9)
#define S_FLAG (1u << 10)
#define E_FLAG (1u << 11)
#define G_FLAG (1u << 12)
#define UNSET   0xFF000000u         /* only a word with all these bits is unset */
#define PRESENT 0x7FFFFFFFu

#define ERR_BAD_STRING     0x0FDu
#define ERR_BUFF_OVERFLOW  0x1E4u
#define ERR_BAD_PARAMETERS 0x1EAu
#define ERR_ARG_REPEATED   0x1EBu

enum { END, FLAG, KEYWORD, ITEM };

struct ra {
    uint32_t keys;                  /* R0 */
    uint32_t p;                     /* the command line, as far as read */
    uint32_t ws, end;               /* the words: R2, and after the last */
    uint32_t next;                  /* the next free byte */
    int32_t left;                   /* R3 */
    uint32_t sp;                    /* the SVC stack, for the kernel's temporaries */
    os_error *e;
};

static uint32_t upper(uint32_t c)
{
    return c >= 'a' && c <= 'z' ? c - 0x20 : c;
}

static int ok_char(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_';
}

static int err(struct ra *ra, uint32_t n, const char *text)
{
    ra->e = ros_error(n, "%s", text);
    return -1;
}

static int overflow(struct ra *ra)
{
    return err(ra, ERR_BUFF_OVERFLOW, "Buffer overflow");
}

/* The kernel's GetArg: which key the option at opt names. The first pass
 * looks for the full name and the second for the first letter only. It
 * returns 0 with the key's number, the end of the name, and whether it was
 * the letter. It returns -1 if no key matches. */
static int get_arg(struct ra *ra, uint32_t opt, uint32_t *after, uint32_t *argno, int *letter)
{
    for (int pass = 0; pass < 2; pass++) {
        uint32_t k = ra->keys, p = opt, n = 0, c, d;
    restart:
        do
            c = ros_ld8(k++);
        while (c == ' ');
        for (;;) {
            if (!ok_char(c)) {
                if (!ok_char(ros_ld8(p))) {             /* the whole name */
                    *after = p, *argno = n, *letter = 0;
                    return 0;
                }
                k--;
                break;
            }
            d = ros_ld8(p++);
            if (upper(c) == upper(d)) {
                c = ros_ld8(k++);
                continue;
            }
            if (pass && p - opt == 2) {                 /* the first letter */
                *after = p - 1, *argno = n, *letter = 1;
                return 0;
            }
            break;
        }
        for (;;) {                                      /* on to the next name */
            c = ros_ld8(k++);
            if (c < ' ')
                break;
            if (c == ',')
                n++;
            if (c == ',' || c == '=') {
                p = opt;
                goto restart;
            }
        }
    }
    return -1;
}

/* The kernel's RdItem: the next thing on the command line. */
static int rd_item(struct ra *ra, uint32_t *val)
{
    uint32_t p = ra->p, c, quote = ' ';
    int demand = 0;
    do
        c = ros_ld8(p++);
    while (c == ' ');
    if (c < ' ')
        return END;
    if (c == '"') {
        quote = '"';
        goto copy;
    }
    if (c == '-') {
        uint32_t dash = p - 1, after, argno;
        int letter;
    option:
        if (get_arg(ra, p, &after, &argno, &letter)) {
            if (demand)
                return err(ra, ERR_BAD_PARAMETERS, "Parameters not recognised");
            p = dash;                   /* an item, '-' and all */
            goto copy;
        }
        p = ra->p = after;
        uint32_t word = ra->ws + 4 * argno, w = ros_ld32(word);
        if (w < UNSET)
            return err(ra, ERR_ARG_REPEATED, "Argument repeated");
        if (!(w & S_FLAG)) {
            *val = argno;
            return KEYWORD;
        }
        ros_st32(word, PRESENT);
        if (letter && ok_char(ros_ld8(p))) {
            demand = 1;                 /* more switches run together */
            goto option;
        }
        return FLAG;
    }
    p--;

copy:
    *val = ra->next;
    for (;;) {
        c = ros_ld8(p++);
        if (c == quote || c < ' ') {
            if (quote != '"')
                break;
            if (c != '"')
                return err(ra, ERR_BAD_STRING, "String not recognised");
            c = ros_ld8(p++);
            if (c != '"')
                break;                  /* the closing quote; "" is a quote */
        }
        if (--ra->left < 0)
            return overflow(ra);
        ros_st8(ra->next++, c);
    }
    if (--ra->left < 0)
        return overflow(ra);
    ros_st8(ra->next++, 0);
    ra->p = p - 1;
    return ITEM;
}

/* The kernel's SetKeyword: the value into its word. It is evaluated or
 * GSTransed in place if the key asks. */
static int set_keyword(struct ra *ra, uint32_t word, uint32_t value)
{
    uint32_t flags = ros_ld32(word);
    ros_st32(word, value);
    if (!(flags & (E_FLAG | G_FLAG)))
        return 0;
    uint32_t len = ra->next - value;
    ra->left += (int32_t)len;
    uint32_t temp = (ra->sp - (len + 11)) & ~3u;        /* the value, out of the way */
    memmove(ros_ptr(temp), ros_ptr(value), len);
    uint32_t outer = ros_svc_sp;
    ros_svc_sp = temp;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    uint32_t buf, n;
    int rc = 0;
    if (flags & E_FLAG) {
        buf = value + 3;
        if ((ra->left -= 3) < 0) {
            rc = overflow(ra);
            goto out;
        }
        c.r[0] = temp, c.r[1] = buf, c.r[2] = (uint32_t)ra->left;
        ros_thunk_OS_EvaluateExpression(&c);
        if (c.v) {
            ra->e = ros_ptr(c.r[0]);
            rc = -1;
            goto out;
        }
        if (c.r[1] == 0) {              /* an integer: type 0, four bytes */
            ros_st8(value, 0);
            if ((ra->left -= 5) < 0) {
                rc = overflow(ra);
                goto out;
            }
            for (int i = 0; i < 4; i++)
                ros_st8(value + 1 + (uint32_t)i, c.r[2] >> (8 * i));
            ra->next = value + 5;
            goto out;
        }
        ros_st8(value, 1);              /* a string: type 1, then as /G */
        c.c = 0;
    } else {
        buf = value + 2;
        if ((ra->left -= 2) < 0) {
            rc = overflow(ra);
            goto out;
        }
        c.r[0] = temp, c.r[1] = buf, c.r[2] = (uint32_t)ra->left | 1u << 31;
        ros_thunk_OS_GSTrans(&c);
        if (c.v) {
            ra->e = ros_ptr(c.r[0]);
            rc = -1;
            goto out;
        }
    }
    n = c.r[2];
    ra->left -= (int32_t)n;
    ra->next = buf + n;
    ros_st8(buf - 2, n);
    ros_st8(buf - 1, n >> 8);
    if (c.c)
        rc = overflow(ra);
out:
    ros_svc_sp = outer;
    return rc;
}

static int read_args(struct ra *ra)
{
    /* A word per key, and its qualifiers */
    uint32_t k = ra->keys, c;
    ra->next = ra->ws;
    do {
        if ((ra->left -= 4) < 0)
            return overflow(ra);
        ros_st32(ra->next, UNSET);
        ra->next += 4;
        for (;;) {
            c = ros_ld8(k++);
            if (c == '/') {
                c = upper(ros_ld8(k++));
                c = c == 'A' ? A_FLAG : c == 'K' ? K_FLAG : c == 'S' ? S_FLAG
                  : c == 'E' ? E_FLAG : c == 'G' ? G_FLAG : c;
                if (c >= 256)
                    ros_st32(ra->next - 4, ros_ld32(ra->next - 4) | c);
            }
            if (c == ',' || c < ' ')
                break;
        }
    } while (c == ',');
    ra->end = ra->next;

    for (;;) {
        uint32_t v, v2;
        int t = rd_item(ra, &v);
        if (t < 0)
            return -1;
        if (t == END)
            break;
        if (t == KEYWORD) {
            int t2 = rd_item(ra, &v2);
            if (t2 < 0)
                return -1;
            if (t2 != ITEM)
                return err(ra, ERR_BAD_PARAMETERS, "Parameters not recognised");
            if (set_keyword(ra, ra->ws + 4 * v, v2))
                return -1;
        } else if (t == ITEM) {
            uint32_t w = ra->ws;        /* the next positional key */
            for (; w < ra->end; w += 4) {
                uint32_t f = ros_ld32(w);
                if (f >= UNSET && !(f & (K_FLAG | S_FLAG)))
                    break;
            }
            if (w == ra->end)
                return err(ra, ERR_BAD_PARAMETERS, "Parameters not recognised");
            if (set_keyword(ra, w, v))
                return -1;
        }
    }

    for (uint32_t w = ra->ws; w < ra->end; w += 4) {    /* what was not given */
        uint32_t f = ros_ld32(w);
        if (f < UNSET)
            continue;
        if (f & A_FLAG)
            return err(ra, ERR_BAD_PARAMETERS, "Parameters not recognised");
        ros_st32(w, 0);
    }
    return 0;
}

void ros_thunk_OS_ReadArgs(struct ros_cpu *s)
{
    struct ra ra = { .keys = s->r[0], .p = s->r[1], .ws = s->r[2],
                     .left = (int32_t)s->r[3], .sp = s->r[13] };
    uint32_t outer = ros_svc_sp_enter(s);
    s->v = 0;
    if (read_args(&ra))
        ros_swi_fail(s, ra.e);
    else
        s->r[3] = (uint32_t)ra.left;
    ros_svc_sp = outer;
}
