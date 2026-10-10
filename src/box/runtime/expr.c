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
 * (Sources/Kernel: s.Arthur3).
 */

/* expr.c: OS_EvaluateExpression, reimplemented.
 *
 * Written from the kernel's ReadExpression (Kernel/s/Arthur3).  It has the
 * same two steps and gives the same answers.
 *
 *   1. The expression is copied into a buffer, expanding <variables>
 *      through OS_GSRead outside quotes and leaving quoted strings as they
 *      are.  This lets "SetEval x <Zap$OSVsn>" work without mangling
 *      "<Alias$@RunType_FFB>"="" (the kernel's own note).
 *   2. An operator-precedence parse of that buffer.  It uses a stack of
 *      the kernel's size, the kernel's two precedence tables, and its
 *      operators and conversions.  The values are integers and strings.
 *      + joins strings.  Strings are converted to integers where an
 *      integer is wanted, and back again for STR, LEFT and RIGHT.  Names
 *      are read as system variables.  Quoted strings are passed through
 *      GSTrans.
 *
 * The errors are the kernel's, with its numbers and texts.  The registers
 * are left as its paths leave them.  R1 is 0 after most errors.  R1-R4 are
 * as they were when the expression could not be copied or a quoted string
 * overflowed.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define LONG_CLI 1024u
#define STACK_BYTES (0x8000u - 0x44C8u)     /* ExprStackStart - ExprStackLimit */

#define GS_NO_QUOTE       (1u << 31)
#define GS_NO_VBAR        (1u << 30)
#define GS_SPC_TERM       (1u << 29)
#define GS_READING_STRING (1u << 27)
#define GS_MACROING       (1u << 26)

#define ERR_BAD_BRA       0x162u
#define ERR_STK_OFLO      0x163u
#define ERR_MISS_OPN      0x164u
#define ERR_MISS_OPR      0x165u
#define ERR_BAD_INT       0x166u
#define ERR_STR_OFLO      0x167u
#define ERR_NAFF_ITM      0x168u
#define ERR_DIV_ZERO      0x169u
#define ERR_BUFF_OVERFLOW 0x1E4u

enum { T_INT, T_STR, T_OP };

/* The kernel's operator numbers: single characters are themselves. */
enum {
    OP_BRA = '(', OP_KET = ')', OP_TIMES = '*', OP_PLUS = '+', OP_NE = 44, OP_MINUS = '-',
    OP_STR = 46, OP_DIVIDE = '/', OP_GE = 48, OP_LE = 49, OP_RSHIFT = 50, OP_LSHIFT = 51,
    OP_AND = 52, OP_OR = 53, OP_EOR = 54, OP_NOT = 55, OP_RIGHT = 56, OP_LEFT = 57,
    OP_MOD = 58, OP_BOTTOM = 59, OP_LT = '<', OP_EQ = '=', OP_GT = '>', OP_VAL = 63,
    OP_LRSHIFT = 64, OP_LEN = 65, OP_UPLUS = 66, OP_UMINUS = 67
};

/* Indexed by op - '(' */
static const uint8_t left_prec[] = {
    2, 1, 8, 7, 6, 7, 9, 8, 6, 6, 6, 6, 5, 4, 4, 9, 9, 9, 8, 1, 6, 6, 6, 9, 6, 9, 9, 9,
};
static const uint8_t right_prec[] = {
    11, 0, 7, 6, 5, 6, 10, 7, 5, 5, 5, 5, 4, 3, 3, 10, 10, 10, 7, 1, 5, 5, 5, 10, 5, 10, 10, 10,
};

/* In the kernel's order: longer forms before their prefixes. */
static const struct { const char *text; uint8_t op; } operators[] = {
    { "(", OP_BRA },  { ")", OP_KET },    { "+", OP_PLUS },    { "-", OP_MINUS },
    { "*", OP_TIMES }, { "/", OP_DIVIDE }, { "=", OP_EQ },     { "<>", OP_NE },
    { "<=", OP_LE },  { "<<", OP_LSHIFT }, { "<", OP_LT },     { ">=", OP_GE },
    { ">>>", OP_LRSHIFT }, { ">>", OP_RSHIFT }, { ">", OP_GT }, { "AND", OP_AND },
    { "OR", OP_OR },  { "EOR", OP_EOR },  { "NOT", OP_NOT },   { "RIGHT", OP_RIGHT },
    { "LEFT", OP_LEFT }, { "MOD", OP_MOD }, { "STR", OP_STR }, { "VAL", OP_VAL },
    { "LEN", OP_LEN },
};

/* Characters from ! to ? a name may hold (the kernel's terminatename_map). */
static const char name_map[] = "10111110000101011111111111100001";

/* The kernel's workspace, in the arena: SWIs read from it. */
struct xws {
    char buff[LONG_CLI];            /* ExprBuff: the expression, expanded */
    char acc[LONG_CLI + 4];         /* exprSTRACC */
    char num[LONG_CLI + 4];         /* a string being read as a number */
};

static struct xws *xws;

struct item {
    uint8_t type;
    int32_t v;                      /* the integer, the string's length, or the op */
    char *s;
};

struct ev {
    struct item *st;
    unsigned n, cap;
    uint32_t used;                  /* bytes of the kernel's stack in use */
    uint32_t tos;
    unsigned brackets;
    const char *p;                  /* in xws->buff */
    char **allocs;
    unsigned nallocs;
    os_error *e;
    int restore;                    /* the error leaves R1-R4 as they were */
    jmp_buf jb;
};

__attribute__((noreturn)) static void raise_err(struct ev *ev, os_error *e, int restore)
{
    ev->e = e;
    ev->restore = restore;
    ros_resume_unwind(ev);
    longjmp(ev->jb, 1);
}

__attribute__((noreturn)) static void fail(struct ev *ev, uint32_t n, const char *text)
{
    raise_err(ev, ros_error(n, "%s", text), 0);
}

static char *str_new(struct ev *ev, const char *src, uint32_t n)
{
    char *b = malloc(n + 1);
    char **a = realloc(ev->allocs, (ev->nallocs + 1) * sizeof *a);
    if (!b || !a) {
        free(b);
        if (a)
            ev->allocs = a;
        fail(ev, ERR_STK_OFLO, "Expression stack overflow");
    }
    ev->allocs = a;
    ev->allocs[ev->nallocs++] = b;
    if (n && src)
        memcpy(b, src, n);
    b[n] = 0;
    return b;
}

static uint32_t cost(const struct item *it)
{
    return 8 + (it->type == T_STR ? (((uint32_t)it->v + 3) & ~3u) : 0);
}

static void push(struct ev *ev, struct item it)
{
    if (ev->n == ev->cap) {
        unsigned cap = ev->cap ? ev->cap * 2 : 64;
        struct item *st = realloc(ev->st, cap * sizeof *st);
        if (!st)
            fail(ev, ERR_STK_OFLO, "Expression stack overflow");
        ev->st = st, ev->cap = cap;
    }
    ev->st[ev->n++] = it;
    ev->used += cost(&it);
    if (ev->used >= STACK_BYTES)
        fail(ev, ERR_STK_OFLO, "Expression stack overflow");
}

static struct item pop(struct ev *ev)
{
    if (!ev->n)
        fail(ev, ERR_MISS_OPN, "Missing operand");
    struct item it = ev->st[--ev->n];
    ev->used -= cost(&it);
    return it;
}

static struct item *top(struct ev *ev)
{
    if (!ev->n)
        fail(ev, ERR_MISS_OPR, "Missing operator");
    return &ev->st[ev->n - 1];
}

static struct item integer(int32_t v)
{
    return (struct item){ T_INT, v, NULL };
}

static struct item string(struct ev *ev, const char *s, uint32_t n)
{
    return (struct item){ T_STR, (int32_t)n, str_new(ev, s, n) };
}

/* ---- conversions -------------------------------------------------------------- */

/* The kernel's StringToInteger.  It accepts spaces, a sign, a number as
 * OS_ReadUnsigned reads it, spaces, and nothing else.  An empty or blank
 * string is 0. */
static int32_t to_int(struct ev *ev, const struct item *it)
{
    if (it->type == T_INT)
        return it->v;
    uint32_t n = (uint32_t)it->v;
    memcpy(xws->num, it->s, n);
    xws->num[n] = 13;
    uint32_t p = 0;
    while (xws->num[p] == ' ')
        p++;
    int neg = xws->num[p] == '-';
    if (neg || xws->num[p] == '+')
        p++;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = ros_addr(xws->num) + p, c.r[4] = 0;
    ros_thunk_OS_ReadUnsigned(&c);
    uint32_t q = c.r[1] - ros_addr(xws->num);
    while (xws->num[q] == ' ')
        q++;
    if (q != n)
        fail(ev, ERR_BAD_INT, "String is not convertible to integer");
    int32_t v = (int32_t)c.r[2];
    return neg ? (int32_t)(0u - (uint32_t)v) : v;
}

static struct item to_str(struct ev *ev, const struct item *it)
{
    if (it->type == T_STR)
        return *it;
    char d[16];
    int n = snprintf(d, sizeof d, "%d", it->v);
    return string(ev, d, (uint32_t)n);
}

/* ---- the operators ---------------------------------------------------------------- */

static struct item left_operand(struct ev *ev)
{
    struct item l = pop(ev);
    if (l.type == T_OP)
        fail(ev, ERR_MISS_OPN, "Missing operand");
    return l;
}

/* The kernel's shifts.  An amount of 32 or more shifts everything out.
 * Below that it is an ARM register shift, which uses the amount's bottom
 * byte (so minint, negated, shifts by 0). */
static uint32_t shift(uint32_t v, int32_t by, int kind)
{
    uint32_t n = by >= 32 ? 32 : (uint32_t)by & 0xFF;
    switch (kind) {
    case 0: return n >= 32 ? 0 : v << n;                                    /* LSL */
    case 1: return n >= 32 ? 0 : v >> n;                                    /* LSR */
    default: return n >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> n);
    }
}

/* Two strings as the kernel compares them: bytes, then lengths. */
static int compare(const struct item *a, const struct item *b)
{
    uint32_t na = (uint32_t)a->v, nb = (uint32_t)b->v, n = na < nb ? na : nb;
    for (uint32_t i = 0; i < n; i++)
        if (a->s[i] != b->s[i])
            return (uint8_t)a->s[i] < (uint8_t)b->s[i] ? -1 : 1;
    return (int32_t)na < (int32_t)nb ? -1 : (int32_t)na > (int32_t)nb;
}

static struct item apply(struct ev *ev, uint32_t op, struct item r)
{
    struct item l;
    int32_t a, b;
    int cmp;
    switch (op) {
    case OP_BRA:
        fail(ev, ERR_BAD_BRA, "Mismatched brackets");
    case OP_VAL:
    case OP_UPLUS:
        return integer(to_int(ev, &r));
    case OP_STR:
        return to_str(ev, &r);
    case OP_LEN:
        return integer(to_str(ev, &r).v);
    case OP_NOT:
        return integer((int32_t)~(uint32_t)to_int(ev, &r));
    case OP_UMINUS:
        return integer((int32_t)(0u - (uint32_t)to_int(ev, &r)));

    case OP_PLUS:
        l = left_operand(ev);
        if (r.type == T_STR && l.type == T_STR) {
            uint32_t n = (uint32_t)l.v + (uint32_t)r.v;
            if (n >= LONG_CLI)
                fail(ev, ERR_STR_OFLO, "String too long");
            struct item s = string(ev, NULL, n);
            memcpy(s.s, l.s, (uint32_t)l.v);
            memcpy(s.s + l.v, r.s, (uint32_t)r.v);
            return s;
        }
        return integer((int32_t)((uint32_t)to_int(ev, &l) + (uint32_t)to_int(ev, &r)));

    case OP_EQ: case OP_NE: case OP_GT: case OP_LT: case OP_GE: case OP_LE:
        l = left_operand(ev);
        if (r.type == T_STR && l.type == T_STR) {
            cmp = compare(&l, &r);
        } else {
            a = to_int(ev, &l), b = to_int(ev, &r);
            cmp = a < b ? -1 : a > b;
        }
        switch (op) {
        case OP_EQ: return integer(cmp == 0 ? -1 : 0);
        case OP_NE: return integer(cmp != 0 ? -1 : 0);
        case OP_GT: return integer(cmp > 0 ? -1 : 0);
        case OP_LT: return integer(cmp < 0 ? -1 : 0);
        case OP_GE: return integer(cmp >= 0 ? -1 : 0);
        default: return integer(cmp <= 0 ? -1 : 0);
        }

    case OP_RIGHT:
    case OP_LEFT: {
        uint32_t want = (uint32_t)to_int(ev, &r);
        l = left_operand(ev);
        l = to_str(ev, &l);
        uint32_t have = (uint32_t)l.v;
        if (want >= have)
            return l;               /* as it is: more than there is, or negative */
        if (op == OP_LEFT)
            return string(ev, l.s, want);
        return string(ev, l.s + have - want, want);
    }
    default:
        break;
    }

    /* the rest take two integers, the right one converted first */
    b = to_int(ev, &r);
    l = left_operand(ev);
    a = to_int(ev, &l);
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    switch (op) {
    case OP_MINUS: return integer((int32_t)(ua - ub));
    case OP_TIMES: return integer((int32_t)(ua * ub));
    case OP_AND: return integer((int32_t)(ua & ub));
    case OP_OR: return integer((int32_t)(ua | ub));
    case OP_EOR: return integer((int32_t)(ua ^ ub));
    case OP_DIVIDE:
    case OP_MOD: {
        if (b == 0)
            fail(ev, ERR_DIV_ZERO, "Division by zero");
        /* The magnitudes are divided.  DIV is negative if the signs
         * differ.  MOD takes the dividend's sign. */
        uint32_t m = b < 0 ? 0u - ub : ub, d = a < 0 ? 0u - ua : ua;
        int neg = op == OP_MOD ? a < 0 : (a < 0) != (b < 0);
        uint32_t q = op == OP_MOD ? d % m : d / m;
        return integer((int32_t)(neg ? 0u - q : q));
    }
    case OP_RSHIFT:
        return integer((int32_t)(b < 0 ? shift(ua, (int32_t)(0u - ub), 0) : shift(ua, b, 2)));
    case OP_LRSHIFT:
        return integer((int32_t)(b < 0 ? shift(ua, (int32_t)(0u - ub), 0) : shift(ua, b, 1)));
    default:                        /* OP_LSHIFT */
        return integer((int32_t)(b < 0 ? shift(ua, (int32_t)(0u - ub), 2) : shift(ua, b, 0)));
    }
}

static void compile_top_op(struct ev *ev)
{
    struct item r = pop(ev);
    if (r.type == T_OP)
        fail(ev, ERR_MISS_OPN, "Missing operand");
    struct item op = pop(ev);
    if (op.type != T_OP)
        fail(ev, ERR_MISS_OPR, "Missing operator");
    struct item v = apply(ev, (uint32_t)op.v, r);
    struct item *t = top(ev);
    if (t->type != T_OP)
        fail(ev, ERR_MISS_OPR, "Missing operator");
    ev->tos = (uint32_t)t->v;
    push(ev, v);
}

/* ---- the items ------------------------------------------------------------------ */

static int name_char(uint32_t c)
{
    if (c >= '@')
        return 1;
    if (c <= ' ')
        return 0;
    return name_map[c - '!'] == '1';
}

/* A system variable's value: a number, or a string (a macro expanded). */
static void variable(struct ev *ev, uint32_t name)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = name, c.r[1] = ros_addr(xws->acc), c.r[2] = (uint32_t)-1, c.r[3] = 0, c.r[4] = 0;
    ros_thunk_OS_ReadVarVal(&c);
    if (c.r[2] == 0)
        fail(ev, ERR_NAFF_ITM, "Unknown operand");
    uint32_t type = c.r[4];
    ros_cpu_enter(&c);
    c.r[0] = name, c.r[1] = ros_addr(xws->acc), c.r[2] = LONG_CLI - 1, c.r[3] = 0;
    c.r[4] = type == 2 ? 3 : type;
    ros_thunk_OS_ReadVarVal(&c);
    if (c.v)
        fail(ev, ERR_STR_OFLO, "String too long");
    if (c.r[4] == 1)
        push(ev, integer((int32_t)ros_ld32(ros_addr(xws->acc))));
    else
        push(ev, string(ev, xws->acc, c.r[2]));
}

/* The kernel's GetFactor: an operator's number, or -1 with a value pushed. */
static int get_factor(struct ev *ev)
{
    uint8_t c;
    while ((c = (uint8_t)*ev->p++) == ' ')
        ;
    if (c == 13)
        return OP_BOTTOM;

    if (c == '&' || (c >= '0' && c <= '9')) {
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[0] = 10, r.r[1] = ros_addr(ev->p - 1), r.r[4] = 0;
        ros_thunk_OS_ReadUnsigned(&r);
        if (r.v)
            raise_err(ev, ros_ptr(r.r[0]), 0);
        ev->p = ros_ptr(r.r[1]);
        push(ev, integer((int32_t)r.r[2]));
        return -1;
    }

    if (c == '"') {
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[0] = ros_addr(ev->p - 1), r.r[1] = ros_addr(xws->acc);
        r.r[2] = LONG_CLI | GS_NO_VBAR | GS_SPC_TERM;
        ros_thunk_OS_GSTrans(&r);
        if (r.c)
            raise_err(ev, ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow"), 1);
        if (r.v)
            raise_err(ev, ros_ptr(r.r[0]), 0);
        ev->p = ros_ptr(r.r[0]);
        push(ev, string(ev, xws->acc, r.r[2]));
        return -1;
    }

    for (unsigned i = 0; i < sizeof operators / sizeof operators[0]; i++) {
        const char *t = operators[i].text;
        if ((uint8_t)t[0] != c)
            continue;
        size_t n = strlen(t);
        if (strncmp(ev->p, t + 1, n - 1) == 0) {
            ev->p += n - 1;
            return operators[i].op;
        }
    }

    /* A name: a system variable */
    if (!name_char(c))
        fail(ev, ERR_NAFF_ITM, "Unknown operand");
    uint32_t n = 0;
    xws->num[n++] = (char)c;
    while (name_char((uint8_t)*ev->p) && n < LONG_CLI)
        xws->num[n++] = *ev->p++;
    xws->num[n] = 13;
    variable(ev, ros_addr(xws->num));
    return -1;
}

/* ---- the parse --------------------------------------------------------------------- */

static struct item evaluate(struct ev *ev)
{
    push(ev, (struct item){ T_OP, OP_BOTTOM, NULL });
    ev->tos = OP_BOTTOM;
    for (;;) {
        int op = get_factor(ev);
        if (op < 0)
            continue;
        if (op == OP_KET) {
            if (ev->brackets == 0)
                fail(ev, ERR_BAD_BRA, "Mismatched brackets");
            ev->brackets--;
            while (ev->tos != OP_BRA)
                compile_top_op(ev);
            struct item v = pop(ev);
            if (v.type == T_OP)
                fail(ev, ERR_MISS_OPN, "Missing operand");
            if (pop(ev).type != T_OP)
                fail(ev, ERR_MISS_OPR, "Missing operator");
            struct item *t = top(ev);
            if (t->type != T_OP)
                fail(ev, ERR_MISS_OPR, "Missing operator");
            ev->tos = (uint32_t)t->v;
            push(ev, v);
            continue;
        }
        if (op == OP_BRA)
            ev->brackets++;
        if ((op == OP_PLUS || op == OP_MINUS) && top(ev)->type == T_OP)
            op = op == OP_PLUS ? OP_UPLUS : OP_UMINUS;
        while (left_prec[ev->tos - OP_BRA] > right_prec[op - OP_BRA])
            compile_top_op(ev);
        push(ev, (struct item){ T_OP, op, NULL });
        ev->tos = (uint32_t)op;
        if (op == OP_BOTTOM)
            break;
    }
    pop(ev);                        /* the bottom just pushed */
    struct item v = pop(ev);
    if (v.type == T_OP)
        fail(ev, ERR_MISS_OPN, "Missing operand");
    if (pop(ev).type != T_OP)
        fail(ev, ERR_MISS_OPN, "Missing operand");
    return v;
}

/* Step 1: copy the expression into ExprBuff, CR-terminated, with
 * <variables> expanded outside quotes. */
static void expand(struct ev *ev, uint32_t expr)
{
    struct ros_cpu g;
    ros_cpu_enter(&g);
    g.r[0] = expr, g.r[2] = GS_NO_QUOTE | GS_NO_VBAR;
    ros_thunk_OS_GSInit(&g);
    if (g.v)
        raise_err(ev, ros_ptr(g.r[0]), 1);
    int32_t room = (int32_t)LONG_CLI - 2;
    int outside = 1;
    char *out = xws->buff;
    for (;;) {
        uint32_t c;
        if ((g.r[2] & (GS_READING_STRING | GS_MACROING)) || outside) {
            ros_thunk_OS_GSRead(&g);
            if (g.v)
                raise_err(ev, ros_ptr(g.r[0]), 1);
            if (g.c)
                break;
            c = g.r[1];
        } else {
            c = ros_ld8(g.r[0]++);
        }
        c &= 0xFF;
        if (c == 13 || c == 10 || c == 0)
            break;
        *out++ = (char)c;
        if (--room < 0)
            raise_err(ev, ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow"), 1);
        if (c == '"')
            outside = !outside;
    }
    *out = 13;
}

void ros_thunk_OS_EvaluateExpression(struct ros_cpu *s)
{
    uint32_t outer = ros_svc_sp_enter(s);
    if (!xws)
        xws = ros_rma_alloc(sizeof *xws);
    struct ev ev = { 0 };
    uint32_t buffer = s->r[1], size = s->r[2];
    s->v = 0;
    if (!setjmp(ev.jb)) {
        expand(&ev, s->r[0]);
        ev.p = xws->buff;
        struct item v = evaluate(&ev);
        if (v.type == T_INT) {
            s->r[1] = 0;
            s->r[2] = (uint32_t)v.v;
        } else {
            uint32_t n = (uint32_t)v.v;
            if ((int32_t)size < (int32_t)n) {
                n = size;
                ros_swi_fail(s, ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow"));
            }
            if ((int32_t)n > 0)
                memcpy(ros_ptr(buffer), v.s, n);
            s->r[1] = buffer;
            s->r[2] = n;
        }
    } else {
        ros_swi_fail(s, ev.e);
        s->r[1] = ev.restore ? buffer : 0;
        s->r[2] = size;
    }
    for (unsigned i = 0; i < ev.nallocs; i++)
        free(ev.allocs[i]);
    free(ev.allocs);
    free(ev.st);
    ros_svc_sp = outer;
}
