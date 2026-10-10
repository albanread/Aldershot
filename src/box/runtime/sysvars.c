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
 * (Sources/Kernel: s.Arthur2, hdr.KernelWS).
 */
/* sysvars.c -- system variables and GSTrans, reimplemented.
 *
 * This follows the kernel's own code (Kernel/s/Arthur2). The kernel keeps
 * the two together because GSTrans reads the variables' structures
 * directly. A <name> in a string expands to the variable's value, and a
 * string variable's value is GSTransed as it is set.
 *
 * The store is the kernel's, in the arena:
 *
 *   - Each variable is one RMA block. It holds the name, 0-terminated, a
 *     type byte, and then the value. A string or macro value is a
 *     three-byte length and the bytes (a macro keeps its terminator after
 *     them). A number is four bytes. Code is a word-aligned block whose
 *     first two words are its write and read entries.
 *   - Zero page's VariableList points at the index. The index is a word
 *     that the kernel keeps for the last context it returned, then the
 *     count, then the nodes' addresses sorted by name, case-insensitively.
 *
 * So what callers depend on holds. OS_ReadVarVal's R3 is the node's name,
 * and passed back it continues a wildcard enumeration. Names are matched
 * without case. * and # are wildcards, with the kernel's backtracking.
 *
 * Code variables. A caller's code block is ARM code, which ROSGD never
 * runs here. The kernel's own code variables, and most in the corpus,
 * begin with a branch to the real routine, either LDR PC, [PC, #n] to a
 * DCD or B. The runtime follows that branch to the compiled or native code
 * it names. Some blocks have entries that are the routines themselves.
 * One is the Wimp's Wimp$State, which is MOV PC,LR followed by its read
 * routine. Such a block is compiled where it was given (rosasm makes the
 * read entry an entry). The copy's two entry words become LDR PC, [PC, #n]
 * to those addresses, kept after the block, and are followed in the same
 * way. A block that begins with anything else cannot be called, and an
 * error says so.
 *
 * OS_SetVarVal's expression type (3) goes through OS_EvaluateExpression
 * (runtime/expr.c). Setting Sys$Time, Sys$Date or Sys$Year changes
 * nothing, because the clock is the host's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/sysvars.h"

enum { VT_STRING = 0, VT_NUMBER = 1, VT_MACRO = 2, VT_EXPANDED = 3, VT_LITERAL = 4, VT_CODE = 16 };

#define ERR_BAD_MAC_VAL   0x120u
#define ERR_BAD_VAR_NAM   0x121u
#define ERR_BAD_VAR_TYPE  0x122u
#define ERR_VAR_NO_ROOM   0x123u
#define ERR_VAR_CANT_FIND 0x124u
#define ERR_VAR_TOO_LONG  0x125u
#define ERR_BAD_STRING    0x0FDu
#define ERR_RC            0x1E2u    /* RCExc and RCNegative share it */
#define ERR_BUFF_OVERFLOW 0x1E4u

/* Kernel/hdr/KernelWS, read by assembling it. */
#define ZP_VARIABLE_LIST (ROS_ZEROPAGE + 0xAB4)
#define ZP_RETURN_CODE   ROS_ZP_RETURN_CODE
#define ZP_RC_LIMIT      ROS_ZP_RC_LIMIT
#define LONG_CLI 1024u              /* LongCLISize: a buffer for an expanded value */

/* GSInit's flags, and GSRead's state in R2 (Arthur2's GS_ bits). */
#define GS_NO_QUOTE       (1u << 31)
#define GS_NO_VBAR        (1u << 30)
#define GS_SPC_TERM       (1u << 29)    /* in R2 after GSInit: set = space does NOT end */
#define GS_IN_STRING      (1u << 28)
#define GS_READING_STRING (1u << 27)
#define GS_MACROING       (1u << 26)
#define GS_LIMIT_POS      19            /* bits 19-25: the stack pointer at GSInit */
#define GS_STACK_LIM      128u

#define INDEX_START 256u            /* SysVars_Vindex_NStart */
#define INDEX_BUMP  32u             /* SysVars_Vindex_NBump */

/* The kernel's workspace for this, in the RMA. */
struct ws {
    uint32_t gs_ptr;                /* GS_StackPtr */
    uint32_t gs_stack[GS_STACK_LIM];/* where each expansion goes back to */
    char gs_name[256];              /* GSNameBuff */
    char work[256];                 /* SysVarWorkSpace: a code variable's value */
    uint8_t code[16];               /* building the kernel's code variables */
};

static struct ws *ws;
static uint32_t capacity;           /* entries the index block holds */

#define ENTER(s)                                                            \
    uint32_t outer_sp_ = ros_svc_sp;                                        \
    ros_svc_sp = (s)->r[13]
#define LEAVE() ros_svc_sp = outer_sp_

static uint32_t upper(uint32_t c)
{
    return c >= 'a' && c <= 'z' ? c - 0x20 : c;
}

static void fail(struct ros_cpu *s, uint32_t errnum, const char *text)
{
    ros_swi_fail(s, ros_error(errnum, "%s", text));
}

/* The error "System variable 'name' not found", with the name up to its
 * terminator. */
static os_error *cant_find(uint32_t name)
{
    char n[256];
    unsigned i = 0;
    for (uint32_t c; i < sizeof n - 1 && (c = ros_ld8(name + i)) > ' '; i++)
        n[i] = (char)c;
    n[i] = 0;
    return ros_error(ERR_VAR_CANT_FIND, "System variable '%s' not found", n);
}

/* ---- the index ------------------------------------------------------------- */

/* The count word. Entries 1 to count follow it. */
static uint32_t *table(void)
{
    uint32_t block = ros_ld32(ZP_VARIABLE_LIST);
    return block ? (uint32_t *)ros_ptr(block + 4) : NULL;
}

static uint32_t count(void)
{
    uint32_t *t = table();
    return t ? t[0] : 0;
}

static void last_context(uint32_t i)
{
    uint32_t block = ros_ld32(ZP_VARIABLE_LIST);
    if (block)
        ros_st32(block, i);
}

/* Makes room for one more entry. It returns 0 on success, or -1 if the
 * index could not grow. */
static int index_room(void)
{
    uint32_t n = count();
    if (ros_ld32(ZP_VARIABLE_LIST) && n < capacity)
        return 0;
    uint32_t cap = capacity ? capacity + INDEX_BUMP : INDEX_START;
    uint8_t *block = ros_rma_alloc(8 + 4 * cap);
    if (!block)
        return -1;
    memset(block, 0, 8 + 4 * cap);
    ((uint32_t *)block)[0] = 0xFFFFFFFFu;       /* no last context */
    uint32_t old = ros_ld32(ZP_VARIABLE_LIST);
    if (old) {
        memcpy(block + 4, ros_ptr(old + 4), 4 + 4 * n);
        ros_rma_free(ros_ptr(old));
    }
    ros_st32(ZP_VARIABLE_LIST, ros_addr(block));
    capacity = cap;
    return 0;
}

/* The kernel's comparison. It is case-insensitive, and any character up to
 * space ends the name. */
static int compare(uint32_t node, uint32_t name)
{
    for (;; node++, name++) {
        uint32_t a = ros_ld8(node), b = ros_ld8(name);
        a = a <= ' ' ? 0 : upper(a);
        b = b <= ' ' ? 0 : upper(b);
        if (a != b)
            return a < b ? -1 : 1;
        if (!a)
            return 0;
    }
}

static int wildcarded(uint32_t name)
{
    for (uint32_t c;; name++) {
        c = ros_ld8(name);
        if (c == '*' || c == '#')
            return 1;
        if (c <= ' ')
            return 0;
    }
}

/* The kernel's WildMatch. The wild name is terminated by any character up
 * to space, and the node's name by 0. * matches any run of characters and
 * # matches any one character. */
static int wild_match(uint32_t w, uint32_t name)
{
    uint32_t wb = 0, nb = 0, c1, c2;
next:
    c1 = ros_ld8(w++);
    if (c1 == '*')
        goto star;
    c2 = ros_ld8(name++);
    if (c2 == 0)
        goto name_ended;
    c1 = upper(c1), c2 = upper(c2);
    if (c1 == c2 || c1 == '#')
        goto next;
    w = wb;
    name = nb;
    if (nb)
        goto star;                  /* backtrack to the last * */
    return 0;
name_ended:
    return c1 <= ' ';
star:
    wb = w;
    c1 = ros_ld8(w++);
    if (c1 == '*')
        goto star;
    c1 = upper(c1);
    for (;;) {
        c2 = ros_ld8(name++);
        if (c2 == 0)
            goto name_ended;
        c2 = upper(c2);
        if (c1 == c2 || c1 == '#')
            break;
    }
    nb = name;
    goto next;
}

struct found {
    uint32_t name;                  /* the node, or 0 */
    uint32_t type;                  /* the address of its type byte */
    uint32_t at;                    /* its index, or where it would go: 1-based */
};

static uint32_t after_name(uint32_t node)
{
    while (ros_ld8(node))
        node++;
    return node + 1;
}

/* The kernel's VarFindIt. With a context, which is a name that a previous
 * call returned, the search goes on after it. Otherwise a wildcarded name
 * is matched from the start, and a plain one is found by binary chop. The
 * binary chop also gives the insertion point. */
static int find(uint32_t name, uint32_t context, struct found *f)
{
    uint32_t *t = table(), n = count(), i = 0;
    f->name = f->type = 0;
    if (context) {
        for (i = n; i >= 1; i--)
            if (t[i] == context)
                goto scan;
    }
    if (wildcarded(name)) {
        i = 0;
        goto scan;
    }
    uint32_t lo = 1, hi = n + 1;    /* the first entry >= name */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (compare(t[mid], name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    f->at = lo;
    if (lo <= n && compare(t[lo], name) == 0) {
        f->name = t[lo];
        f->type = after_name(t[lo]);
        last_context(lo);
        return 1;
    }
    return 0;
scan:
    for (i++; i <= n; i++) {
        if (wild_match(name, t[i])) {
            f->name = t[i];
            f->type = after_name(t[i]);
            f->at = i;
            last_context(i);
            return 1;
        }
    }
    f->at = 1;
    return 0;
}

/* The kernel's VarFindIt_QA, for Oscli's alias lookup. It finds one name,
 * not wildcarded, by binary chop, and returns its node or 0. It keeps no
 * context, and it is not a SWI, so asking whether a variable is there
 * costs nothing and makes no error (Arthur2: "don't want to save context
 * in this version of routine"). */
uint32_t ros_sysvars_find_quick(uint32_t name)
{
    uint32_t *t = table(), n = count(), lo = 1, hi = n + 1;
    if (!t)
        return 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (compare(t[mid], name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo <= n && compare(t[lo], name) == 0 ? t[lo] : 0;
}

static uint32_t ld24(uint32_t a)
{
    return ros_ld8(a) | ros_ld8(a + 1) << 8 | ros_ld8(a + 2) << 16;
}

/* ---- code variables ---------------------------------------------------------- */

/* Where the branch at `at` goes, or 0. The branch is LDR PC, [PC, #+-n] or
 * B. */
static uint32_t branch_target(uint32_t at)
{
    uint32_t w = ros_ld32(at);
    if ((w & 0xFFFFF000u) == 0xE59FF000u)           /* LDR PC, [PC, #n] */
        return ros_ld32(at + 8 + (w & 0xFFF));
    if ((w & 0xFFFFF000u) == 0xE51FF000u)           /* LDR PC, [PC, #-n] */
        return ros_ld32(at + 8 - (w & 0xFFF));
    if ((w & 0xFF000000u) == 0xEA000000u)           /* B */
        return at + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
    return 0;
}

/* Whether a code block of len bytes at value is to be called where it was
 * given. That is so if its entries are not both branches and both are
 * compiled code there. */
static int code_in_place(uint32_t value, uint32_t len)
{
    if (len < 8 || len - 8 > 0xFFF)
        return 0;
    if (branch_target(value) && branch_target(value + 4))
        return 0;
    return ros_code_lookup(value) && ros_code_lookup(value + 4);
}

/* Calls a code variable's entry, 0 for write and 1 for read, with c's
 * registers. It follows the branch that the code begins with. It returns
 * 0, or -1 with V set in c. */
static int call_code(uint32_t type, int entry, struct ros_cpu *c)
{
    uint32_t at = ((type + 1 + 3) & ~3u) + 4u * (uint32_t)entry;
    uint32_t target = branch_target(at);
    if (!target || !ros_code_lookup(target)) {
        ros_swi_fail(c, ros_error(ERR_BAD_VAR_TYPE,
                                  "Code variable at &%08X: no compiled code to call", at));
        return -1;
    }
    c->r[14] = ROS_RETURN_TO_NATIVE;
    c->v = 0;
    ros_call(c, target);
    if (c->r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(c, ROS_RETURN_TO_NATIVE);
    return c->v ? -1 : 0;
}

/* ---- GSInit, GSRead, GSTrans --------------------------------------------------- */

static int is_end(uint32_t c)
{
    return c == 13 || c == 10 || c == 0;
}

static uint32_t gs_limit(uint32_t f)
{
    return (f >> GS_LIMIT_POS) & (GS_STACK_LIM - 1);
}

/* OS_GSInit. It takes the string and the caller's flags. It returns the
 * first non-space character and whether the string is empty. */
static void gs_init(uint32_t *p, uint32_t *flags, uint32_t *first, int *empty)
{
    uint32_t a = *p, f = *flags & (GS_NO_QUOTE | GS_NO_VBAR | GS_SPC_TERM), c;
    /* Only a string that may expand something marks the stack's base. */
    for (uint32_t q = a; !is_end(c = ros_ld8(q)); q++) {
        if (c == '<') {
            ws->gs_ptr &= GS_STACK_LIM - 1;
            f |= ws->gs_ptr << GS_LIMIT_POS;
            break;
        }
    }
    f ^= GS_SPC_TERM;
    do
        c = ros_ld8(a++);
    while (c == ' ');
    if (!(f & GS_NO_QUOTE) && c == '"')
        f |= GS_IN_STRING;
    else
        a--;
    *p = a, *flags = f, *first = c;
    *empty = is_end(c);
}

static void gs_push(uint32_t a)
{
    ws->gs_stack[ws->gs_ptr] = a;
    ws->gs_ptr = (ws->gs_ptr + 1) & (GS_STACK_LIM - 1);
}

static uint32_t gs_pop(void)
{
    ws->gs_ptr = (ws->gs_ptr - 1) & (GS_STACK_LIM - 1);
    return ws->gs_stack[ws->gs_ptr];
}

/* A number in <...>. The whole name must be a number, in decimal or any
 * form that OS_ReadUnsigned reads. It returns 1 with the value, or 0. */
static int angled_number(uint32_t name, uint32_t *v)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = name, c.r[4] = 0;
    ros_thunk_OS_ReadUnsigned(&c);
    if (c.v || ros_ld8(c.r[1]) != 0)
        return 0;
    *v = c.r[2];
    return 1;
}

/* OS_GSRead. It returns 0 for a character, 1 for the end (C), or -1 for
 * "String not recognised". */
static int gs_read(uint32_t *pp, uint32_t *fp, uint32_t *ch, os_error **err)
{
    uint32_t p = *pp, f = *fp, top = 0, c;
    int end = 0;

    if (f & GS_READING_STRING) {
        f--;
        if (f << (32 - GS_LIMIT_POS)) {
            c = ros_ld8(p++);           /* already expanded */
            goto out;
        }
        goto zero_length;
    }
next:
    c = ros_ld8(p++);
    if (is_end(c)) {
        if (f & GS_MACROING) {          /* out of a macro, a level up */
            p = gs_pop();
            if (ws->gs_ptr == gs_limit(f))
                f &= ~GS_MACROING;
            goto next;
        }
        if (f & GS_IN_STRING)
            goto bad;                   /* no closing quote */
        end = 1;
        goto out;
    }
    if (c == ' ') {
        if (!(f & (GS_IN_STRING | GS_SPC_TERM | GS_MACROING)))
            end = 1;
        goto ret;
    }
    if (c < ' ')
        goto bad;
    if (c == '"') {
        if (!(f & GS_IN_STRING))
            goto ret;
        c = ros_ld8(p++);
        if (c == '"')
            goto ret;                   /* "" in a string is a quote */
        while (c == ' ')
            c = ros_ld8(p++);
        p--;                            /* at what follows the closing quote */
        end = 1;
        goto out;
    }
    if (c == '|' && !(f & GS_NO_VBAR)) {
        c = ros_ld8(p++);
        if (c == '|' || c == '"' || c == '<')
            goto ret;
        if (c == '?') {
            c = 0x7F;
            goto ret;
        }
        if (c == '!') {
            top = 0x80;                 /* the next character, top bit set */
            goto next;
        }
        if (c < ' ')
            goto bad;
        if (c >= 0x7F) {
            if (c > 0x7F)
                c ^= 0x20;
            goto ret;
        }
        if (c == '`')
            c = '_';
        if (c >= '@')
            c &= 0x1F;
        goto ret;
    }
    if (c != '<')
        goto ret;

    /* <name> or <number>: anything else is a plain '<' */
    {
        uint32_t start = p, q = p, n = 0, v, value, len;
        c = ros_ld8(q);
        if (c == '>' || c == ' ') {
            c = '<';
            goto ret;
        }
        for (;;) {
            c = ros_ld8(q++);
            ws->gs_name[n++] = (char)c;
            if (c == '>')
                break;
            if (n == sizeof ws->gs_name || c == ' ' || c < 32) {
                p = start;
                c = '<';
                goto ret;
            }
        }
        ws->gs_name[n - 1] = 0;
        uint32_t name = ros_addr(ws->gs_name);
        if (angled_number(name, &v)) {
            p = q;
            c = v;
            goto ret;
        }
        struct found fd;
        p = q;
        if (!find(name, 0, &fd))
            goto next;                  /* no such variable: nothing */
        if (ws->gs_ptr == ((gs_limit(f) - 1) & (GS_STACK_LIM - 1)))
            goto next;                  /* nested too deep: nothing */
        gs_push(p);
        uint32_t type = ros_ld8(fd.type);
        if (type == VT_CODE) {
            struct ros_cpu cc;
            ros_cpu_enter(&cc);
            if (call_code(fd.type, 1, &cc))
                len = 0;                /* the kernel ignores a failed read */
            else
                value = cc.r[0], len = cc.r[2];
        } else if (type == VT_NUMBER) {
            len = (uint32_t)snprintf(ws->gs_name, sizeof ws->gs_name, "%d",
                                     (int32_t)ros_ld32(fd.type + 1));
            value = name;
        } else if (type == VT_MACRO) {
            p = fd.type + 4;            /* read the macro's text, expanding it */
            f |= GS_MACROING;
            goto next;
        } else {
            len = ld24(fd.type + 1);
            value = fd.type + 4;
        }
        if (len == 0)
            goto zero_length;
        f |= len | GS_READING_STRING;
        c = ros_ld8(value);
        p = value + 1;
        goto ret;
    }

zero_length:
    p = gs_pop();
    f &= ~GS_READING_STRING;
    goto next;
bad:
    *err = ros_error(ERR_BAD_STRING, "String not recognised");
    *pp = p, *fp = f;
    return -1;
ret:
    c |= top;
out:
    *pp = p, *fp = f, *ch = c;
    return end;
}

/* OS_GSTrans into out. The size is in the low 29 bits of r2 and GSInit's
 * flags are above it. The terminator is stored but not counted. It returns
 * NULL or the error. */
static os_error *gs_trans(uint32_t *p, uint32_t out, uint32_t r2, uint32_t *n, int *overflow)
{
    uint32_t q = out, limit = out + (r2 & 0x1FFFFFFFu), f = r2, c;
    int empty, rc;
    os_error *e = NULL;
    gs_init(p, &f, &c, &empty);
    *overflow = 0;
    for (;;) {
        if (q >= limit) {
            *overflow = 1;
            *n = q - out;
            return NULL;
        }
        rc = gs_read(p, &f, &c, &e);
        if (rc < 0) {
            *n = q - out;
            return e;
        }
        ros_st8(q++, c);
        if (rc) {
            *n = q - out - 1;
            return NULL;
        }
    }
}

void ros_thunk_OS_GSInit(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t p = s->r[0], f = s->r[2], c;
    int empty;
    gs_init(&p, &f, &c, &empty);
    s->r[0] = p, s->r[1] = c, s->r[2] = f;
    s->z = (uint32_t)empty;
    s->v = 0;
    LEAVE();
}

void ros_thunk_OS_GSRead(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t p = s->r[0], f = s->r[2], c = 0;
    os_error *e = NULL;
    int rc = gs_read(&p, &f, &c, &e);
    s->r[0] = p, s->r[2] = f;
    if (rc < 0) {
        ros_swi_fail(s, e);
        s->c = 1;
    } else {
        s->r[1] = c;
        s->c = (uint32_t)rc;
        s->v = 0;
    }
    LEAVE();
}

void ros_thunk_OS_GSTrans(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t p = s->r[0], n;
    int overflow;
    os_error *e = gs_trans(&p, s->r[1], s->r[2], &n, &overflow);
    s->r[2] = n;
    if (e) {
        ros_swi_fail(s, e);
    } else {
        s->r[0] = p;
        s->c = (uint32_t)overflow;
        s->v = 0;
    }
    LEAVE();
}

/* ---- OS_ReadVarVal ---------------------------------------------------------------- */

/* Copies n bytes from src into the caller's buffer, as the kernel copies.
 * If there are too many, the result is the overflow error with as many
 * bytes as fit. If R2 < 0 on entry, none are copied and R2 = NOT the
 * length. */
static void copy_out(struct ros_cpu *s, uint32_t src, uint32_t n)
{
    int32_t room = (int32_t)s->r[2];
    int over = (int32_t)n > room;
    if (over)
        n = room < 0 ? ~n : (uint32_t)room;
    s->r[2] = n;
    if ((int32_t)n > 0)
        memmove(ros_ptr(s->r[1]), ros_ptr(src), n);
    if (over)
        fail(s, ERR_BUFF_OVERFLOW, "Buffer overflow");
}

void ros_thunk_OS_ReadVarVal(struct ros_cpu *s)
{
    ENTER(s);
    struct found f;
    uint32_t want = s->r[4], r0 = s->r[0];
    s->v = 0;
    if (!find(s->r[0], s->r[3], &f)) {
        s->r[2] = 0;
        s->r[3] = 0;
        ros_swi_fail(s, cant_find(r0));
        LEAVE();
        return;
    }
    s->r[3] = f.name;
    uint32_t type = ros_ld8(f.type), data = f.type + 1;
    if (type == VT_CODE) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        memcpy(c.r, s->r, 3 * sizeof c.r[0]);
        if (call_code(f.type, 1, &c)) {
            ros_swi_fail(s, ros_ptr(c.r[0]));
        } else {
            s->r[4] = VT_STRING;
            copy_out(s, c.r[0], c.r[2]);
        }
        LEAVE();
        return;
    }
    s->r[4] = type;
    if (want == VT_EXPANDED && type == VT_NUMBER) {
        extern void ros_thunk_OS_BinaryToDecimal(struct ros_cpu *);
        struct ros_cpu c = *s;
        c.r[0] = ros_ld32(data);
        ros_thunk_OS_BinaryToDecimal(&c);
        s->r[2] = c.r[2];
        s->r[4] = VT_STRING;
        if (c.v)
            ros_swi_fail(s, ros_ptr(c.r[0]));
    } else if (want == VT_EXPANDED && type == VT_MACRO) {
        if ((int32_t)s->r[2] < 0) {
            fail(s, ERR_BUFF_OVERFLOW, "Buffer overflow");      /* R2 as it was */
        } else {
            uint32_t p = data + 3, n;
            int overflow;
            os_error *e = gs_trans(&p, s->r[1], s->r[2], &n, &overflow);
            s->r[2] = n;
            if (e)
                ros_swi_fail(s, e);
            else if (overflow)
                fail(s, ERR_BUFF_OVERFLOW, "Buffer overflow");
        }
    } else if (type == VT_NUMBER) {
        copy_out(s, data, 4);
    } else {
        copy_out(s, data + 3, ld24(data));
    }
    if (s->v == 0)
        s->r[0] = r0;
    LEAVE();
}

/* ---- OS_SetVarVal ----------------------------------------------------------------- */

static uint32_t name_length(uint32_t name)
{
    uint32_t n = 0;
    while (ros_ld8(name + n) > ' ')
        n++;
    return n;
}

/* Makes a node: the name, the type byte, and len bytes of value laid out
 * by the caller. It returns the node's address and where the value goes. */
static uint8_t *new_node(uint32_t name, uint32_t type, uint32_t len, uint8_t **value)
{
    uint32_t n = name_length(name);
    uint32_t head = n + 2;
    if (type == VT_CODE)
        head = (head + 3) & ~3u;
    uint8_t *node = ros_rma_alloc(head + len);
    if (!node)
        return NULL;
    memcpy(node, ros_ptr(name), n);
    node[n] = 0;
    node[n + 1] = (uint8_t)(type == VT_LITERAL ? VT_STRING : type);
    *value = node + head;
    return node;
}

static void put24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16);
}

/* A string's value, GSTransed into a buffer that grows as it needs. */
static os_error *expand(uint32_t value, uint8_t **out, uint32_t *len)
{
    uint32_t p = value, f = 0, c, size = 64, n = 0;
    int empty;
    uint8_t *b = malloc(size);
    if (!b)
        return ros_error(ERR_VAR_NO_ROOM, "No room for this variable");
    gs_init(&p, &f, &c, &empty);
    for (;;) {
        os_error *e = NULL;
        int rc = gs_read(&p, &f, &c, &e);
        if (rc < 0) {
            free(b);
            return e;
        }
        if (rc)
            break;
        if (n == size) {
            uint8_t *nb = realloc(b, size *= 2);
            if (!nb) {
                free(b);
                return ros_error(ERR_VAR_NO_ROOM, "No room for this variable");
            }
            b = nb;
        }
        b[n++] = (uint8_t)c;
    }
    *out = b, *len = n;
    return NULL;
}

/* A value given to a code variable. It is passed as a string in R1 and R2
 * of the variable's write entry. */
static void assign_to_code(struct ros_cpu *s, struct found *f, uint32_t value, uint32_t len,
                           uint32_t type, uint32_t sp)
{
    uint32_t buffer = sp - LONG_CLI;
    ros_svc_sp = buffer;
    if (type == VT_NUMBER) {
        len = (uint32_t)snprintf(ros_ptr(buffer), 256, "%d", (int32_t)ros_ld32(value));
        value = buffer;
    } else if (type == VT_STRING) {
        uint32_t p = value, n;
        int overflow;
        os_error *e = gs_trans(&p, buffer, LONG_CLI, &n, &overflow);
        if (e) {
            ros_swi_fail(s, e);
            return;
        }
        if (overflow) {
            fail(s, ERR_VAR_TOO_LONG, "Variable value too long");
            return;
        }
        value = buffer, len = n;
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = f->name, c.r[1] = value, c.r[2] = len;
    if (call_code(f->type, 0, &c))
        ros_swi_fail(s, ros_ptr(c.r[0]));
}

/* An ARM shadow that is initialising cannot change a variable that already
 * exists. The write, or the deletion, is ignored and succeeds. The shadow
 * may make new variables. The shadow's code is the ARM code running while
 * it initialises. Its SWIs are ARM calls, and native code that it calls
 * makes native ones. */
static int frozen(void)
{
    return ros_module_shadow_initialising() && ros_caller_kind() == ROS_KIND_ARM;
}

static void delete_var(struct ros_cpu *s)
{
    struct found f;
    if (!find(s->r[0], s->r[3], &f)) {
        ros_swi_fail(s, cant_find(s->r[0]));
        return;
    }
    s->r[3] = f.name;
    if (frozen())
        return;
    if (ros_ld8(f.type) == VT_CODE && s->r[4] != VT_CODE)
        return;                         /* only R4 = 16 deletes a code variable */
    uint32_t *t = table(), n = t[0];
    memmove(&t[f.at], &t[f.at + 1], 4 * (n - f.at));
    t[0] = n - 1;
    ros_rma_free(ros_ptr(f.name));
    s->r[3] = f.at > 1 ? t[f.at - 1] : 0;       /* go on from the one before */
}

void ros_thunk_OS_SetVarVal(struct ros_cpu *s)
{
    ENTER(s);
    uint32_t name = s->r[0], value = s->r[1], len = s->r[2], type = s->r[4];
    uint32_t sp = s->r[13];
    s->v = 0;
    if ((int32_t)len < 0) {
        delete_var(s);
        LEAVE();
        return;
    }
    if (type > VT_LITERAL && type != VT_CODE) {
        fail(s, ERR_BAD_VAR_TYPE, "Bad variable type");
        LEAVE();
        return;
    }
    if (type == VT_EXPANDED) {
        /* An expression: a number, or a string (OS_EvaluateExpression) */
        struct ros_cpu c;
        sp -= LONG_CLI;
        ros_svc_sp = sp;
        ros_cpu_enter(&c);
        c.r[0] = value, c.r[1] = sp, c.r[2] = LONG_CLI;
        ros_swi(&c, XOS_EvaluateExpression);
        if (c.v) {
            ros_swi_fail(s, ros_ptr(c.r[0]));
            LEAVE();
            return;
        }
        if (c.r[1] == 0) {
            type = VT_NUMBER;
            ros_st32(sp, c.r[2]);
            len = 4;
        } else {
            type = VT_LITERAL;
            len = c.r[2];
        }
        value = sp;
        s->r[4] = type;
    }

    struct found f;
    if (find(name, s->r[3], &f)) {
        name = f.name;                  /* its own name, not the wildcard */
        if (frozen()) {
            s->r[3] = f.name;
            LEAVE();
            return;
        }
        if (ros_ld8(f.type) == VT_CODE && type != VT_CODE) {
            assign_to_code(s, &f, value, len, type, sp);
            LEAVE();
            return;
        }
    } else if (wildcarded(name) || name_length(name) == 0) {
        fail(s, ERR_BAD_VAR_NAM, "Variable name not recognised");
        LEAVE();
        return;
    }

    uint8_t *node = NULL, *v, *text = NULL;
    os_error *e = NULL;
    switch (type) {
    case VT_STRING:
        if ((e = expand(value, &text, &len)))
            break;
        if ((node = new_node(name, type, 3 + len, &v))) {
            put24(v, len);
            memcpy(v + 3, text, len);
        }
        free(text);
        break;
    case VT_NUMBER:
        if ((node = new_node(name, type, 4, &v)))
            memcpy(v, ros_ptr(value), 4);
        break;
    case VT_MACRO: {
        uint32_t n = 0, c;
        while ((c = ros_ld8(value + n)) >= ' ')
            n++;
        if (c != 0 && c != 10 && c != 13) {
            e = ros_error(ERR_BAD_MAC_VAL, "Bad macro value");
            break;
        }
        if ((node = new_node(name, type, 3 + n + 1, &v))) {
            put24(v, n);                /* the terminator is kept, not counted */
            memcpy(v + 3, ros_ptr(value), n + 1);
        }
        break;
    }
    case VT_LITERAL:
        if ((node = new_node(name, type, 3 + len, &v))) {
            put24(v, len);
            memcpy(v + 3, ros_ptr(value), len);
        }
        break;
    default: {                          /* VT_CODE: len bytes of it */
        /* The entries run where they were compiled. The copy's entry
         * words branch there, through two words after the block. */
        int in_place = code_in_place(value, len);
        if ((node = new_node(name, type, len + (in_place ? 8 : 0), &v))) {
            memcpy(v, ros_ptr(value), len);
            if (in_place) {
                uint32_t at = ros_addr(v);
                ros_st32(at, 0xE59FF000u | (len - 8));          /* LDR PC, [PC, #len-8] */
                ros_st32(at + 4, 0xE59FF000u | (len - 8));
                ros_st32(at + len, value);
                ros_st32(at + len + 4, value + 4);
            }
        }
        break;
    }
    }
    if (!e && !node)
        e = ros_error(ERR_VAR_NO_ROOM, "No room for this variable");
    if (!e && !f.name && index_room()) {
        ros_rma_free(node);
        e = ros_error(ERR_VAR_NO_ROOM, "No room for this variable");
    }
    if (e) {
        ros_swi_fail(s, e);
        LEAVE();
        return;
    }

    uint32_t *t = table(), addr = ros_addr(node);
    if (f.name) {
        ros_rma_free(ros_ptr(f.name));  /* replaced where it stood */
        t[f.at] = addr;
    } else {
        memmove(&t[f.at + 1], &t[f.at], 4 * (t[0] + 1 - f.at));
        t[f.at] = addr;
        t[0]++;
    }
    s->r[3] = addr;
    LEAVE();
}

/* ---- the kernel's own ----------------------------------------------------------- */

static void done(struct ros_cpu *s)
{
    s->r[15] = s->r[14];
}

static void read_time_format(struct ros_cpu *s, const char *format)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    s->r[2] = (uint32_t)strftime(ws->work, sizeof ws->work, format, &tm);
    s->r[0] = ros_addr(ws->work);
    s->v = 0;
    done(s);
}

/* The kernel formats them through the Territory module. This is its UK
 * format. */
static void read_time(struct ros_cpu *s) { read_time_format(s, "%H:%M:%S"); }
static void read_date(struct ros_cpu *s) { read_time_format(s, "%a,%d %b"); }
static void read_year(struct ros_cpu *s) { read_time_format(s, "%Y"); }

/* Setting the clock. The length is checked as the kernel checks it, and
 * then nothing happens, as OS_Word 15 does nothing. The clock is the
 * host's. */
static void set_clock(struct ros_cpu *s, uint32_t longest)
{
    s->v = 0;
    if (s->r[2] > longest)
        fail(s, ERR_VAR_TOO_LONG, "Variable value too long");
    done(s);
}

static void set_time(struct ros_cpu *s) { set_clock(s, 0xFE); }
static void set_date(struct ros_cpu *s) { set_clock(s, 0xF8); }
static void set_year(struct ros_cpu *s) { set_clock(s, 4); }

static void read_number(struct ros_cpu *s, uint32_t at)
{
    s->r[2] = (uint32_t)snprintf(ws->work, sizeof ws->work, "%d", (int32_t)ros_ld32(at));
    s->r[0] = ros_addr(ws->work);
    s->v = 0;
    done(s);
}

/* The kernel's SetNumSysVar. The value is an optional sign and a number.
 * Anything that does not read as one is 0. */
static int32_t number_value(uint32_t p, uint32_t len)
{
    if (len == 0)
        return 0;
    if (len > sizeof ws->work - 1)
        len = sizeof ws->work - 1;
    memcpy(ws->work, ros_ptr(p), len);
    ws->work[len] = 13;
    uint32_t q = ros_addr(ws->work);
    int neg = ws->work[0] == '-';
    if (neg || ws->work[0] == '+')
        q++;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = q, c.r[4] = 0;
    ros_thunk_OS_ReadUnsigned(&c);
    int32_t v = c.v ? 0 : (int32_t)c.r[2];
    return neg ? -v : v;
}

static void read_rc(struct ros_cpu *s) { read_number(s, ZP_RETURN_CODE); }
static void read_rcl(struct ros_cpu *s) { read_number(s, ZP_RC_LIMIT); }

static void set_rc(struct ros_cpu *s)
{
    int32_t rc = number_value(s->r[1], s->r[2]);
    int32_t limit = (int32_t)ros_ld32(ZP_RC_LIMIT);
    ros_st32(ZP_RETURN_CODE, (uint32_t)rc);
    s->v = 0;
    if ((uint32_t)rc > (uint32_t)limit)
        fail(s, ERR_RC, rc > limit ? "Return code limit exceeded" : "Negative return code");
    done(s);
}

static void set_rcl(struct ros_cpu *s)
{
    int32_t v = number_value(s->r[1], s->r[2]);
    if (v < 0)
        v = v == INT32_MIN ? 0 : -v;    /* it cannot be negative */
    ros_st32(ZP_RC_LIMIT, (uint32_t)v);
    s->v = 0;
    done(s);
}

static void set(const char *name, uint32_t value, uint32_t len, uint32_t type)
{
    struct ros_cpu c;
    char *n = ws->gs_name;
    strcpy(n, name);
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(n), c.r[1] = value, c.r[2] = len, c.r[3] = 0, c.r[4] = type;
    ros_thunk_OS_SetVarVal(&c);
}

static void code_var(const char *name, ros_code *write, ros_code *read)
{
    /* The kernel's own form: LDR PC, [PC, #0] twice, then the two addresses */
    uint32_t w[4] = { 0xE59FF000u, 0xE59FF000u, ros_native_entry(write, name),
                      ros_native_entry(read, name) };
    memcpy(ws->code, w, sizeof w);
    set(name, ros_addr(ws->code), sizeof w, VT_CODE);
}

static void string_var(const char *name, const char *value)
{
    strcpy(ws->work, value);
    set(name, ros_addr(ws->work), 0, VT_STRING);
}

void ros_sysvars_init(void)
{
    ws = ros_rma_alloc(sizeof *ws);
    memset(ws, 0, sizeof *ws);
    capacity = 0;
    ros_st32(ZP_VARIABLE_LIST, 0);
    ros_st32(ZP_RC_LIMIT, 0x100);
    ros_st32(ZP_RETURN_CODE, 0);
    code_var("Sys$Time", set_time, read_time);
    code_var("Sys$Year", set_year, read_year);
    code_var("Sys$Date", set_date, read_date);
    code_var("Sys$ReturnCode", set_rc, read_rc);
    code_var("Sys$RCLimit", set_rcl, read_rcl);
    string_var("Alias$.", "Cat \n");
    string_var("Alias$@RunType_FEB", "Obey %*0\n");
    string_var("Alias$BASIC", "BASICVFP %*0\n");    /* the ROM's only BASIC */
    string_var("Sys$DateFormat", "%24:%mi:%se %dy-%m3-%ce%yr\n");
}
