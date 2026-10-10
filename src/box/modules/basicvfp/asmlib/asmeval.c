/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asmeval.c: the literal evaluator that both encoders share (see asm.h).
 * It accepts hex (with & or 0x), decimal, negative numbers, and an optional
 * leading '#'. Octal is not accepted. It is kept out of the encoders so that
 * a build with only one instruction set still links. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "asm.h"

int asm_eval_literal(void *ud, const char *b, const char *e, int64_t *out,
                     char *err, size_t errcap)
{
    (void)ud;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (b == e) {
        snprintf(err, errcap, "missing value");
        return -1;
    }
    int neg = 0;
    if (*b == '-') { neg = 1; b++; }
    else if (*b == '+') b++;
    while (b < e && isspace((unsigned char)*b)) b++;
    if (b < e && (isalpha((unsigned char)*b) || *b == '_')) {
        /* an identifier, so not a literal */
        return 1;
    }
    /* At most 64 bits, signed or unsigned. 0xFFFFFFFFFFFFFFFF is -1. A
     * larger number gives "number too big". */
    uint64_t v = 0;
    int hex = 0;
    if (b + 2 <= e && b[0] == '0' && (b[1] == 'x' || b[1] == 'X'))
        b += 2, hex = 1;
    else if (b + 1 <= e && b[0] == '&')
        b += 1, hex = 1;
    if (hex) {
        for (; b < e; b++) {
            int h = asm__hexval(*b);
            if (h < 0) { snprintf(err, errcap, "bad hex digit"); return -1; }
            if (v >> 60) { snprintf(err, errcap, "number too big"); return -1; }
            v = (v << 4) | (uint64_t)h;
        }
    } else {
        for (; b < e; b++) {
            if (!isdigit((unsigned char)*b)) {
                snprintf(err, errcap, "bad number '%.*s'", (int)(e - b), b);
                return -1;
            }
            unsigned d = (unsigned)(*b - '0');
            if (v > (UINT64_MAX - d) / 10) { snprintf(err, errcap, "number too big"); return -1; }
            v = v * 10 + d;
        }
    }
    *out = (int64_t)(neg ? 0 - v : v);
    return 0;
}

int asm__hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int asm__fail(asm_ctx *c, int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->ekind = kind;
    return ASM_ESYNTAX;
}

void asm__soft(asm_ctx *c, int kind, const char *fmt, ...)
{
    if (c->soft)
        return;                 /* only the first one is reported */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->softerr, sizeof c->softerr, fmt, ap);
    va_end(ap);
    c->soft = kind;
}

const char *asm__item_end(const char *p, const char *e)
{
    int depth = 0, quote = 0;
    for (; p < e; p++) {
        if (quote) {
            if (*p == '"')
                quote = 0;
            continue;
        }
        if (*p == '"')
            quote = 1;
        else if (*p == '[' || *p == '(' || *p == '{')
            depth++;
        else if (*p == ']' || *p == ')' || *p == '}')
            depth--;
        else if (*p == ',' && depth <= 0)
            return p;
    }
    return e;
}
