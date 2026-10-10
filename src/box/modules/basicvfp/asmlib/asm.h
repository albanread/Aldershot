/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asm.h: the interface shared by the two cross-assemblers.
 *
 * Both encoders (asma64.c and asmx64.c) take one instruction of assembly
 * text and produce bytes at a known address.  The encoders never parse
 * numeric operand text.  They hand it to an eval callback.  The same
 * encoders therefore serve the corpus tests, which use plain literals, and
 * BBC BASIC, which uses its own expressions.  In BASIC the labels are BASIC
 * variables, and a name not made yet reads as P%.
 *
 * The encodings are byte-identical to LLVM MC.  The corpora in oracles/MRASM
 * (aarch64.tsv, from MRASM/JASM) and oracles/WRASM (x86_64.tsv) check this.
 */
#ifndef BASICASM_ASM_H
#define BASICASM_ASM_H

#include <stddef.h>
#include <stdint.h>

/* The return value of an eval callback:
 *   0 means *out is set.
 *   1 means the value is not known yet. This is a forward reference that
 *     the caller cannot give any value at all.
 *   2 means *out is set, but the value is not settled. Another pass may see
 *     it differently. This covers a name not made yet (BASIC gives P%) and
 *     any variable, FN or indirection other than a label made earlier in
 *     the same '[' (see basicasm.c). x86-64 then takes the long form of
 *     whatever depends on the value (a jump, an immediate or a
 *     displacement), so the code is the same size in every pass.
 *   A negative value is a hard error, with the message in err.
 * ASM_EVAL_NAMED may be or'd into 0 or 2. It means the value names
 * something, as an MC symbol does (a label or a variable), and is not a
 * plain number. For x86-64, [rip + label] is the label's address and
 * [rip + 16] is a displacement. */
#define ASM_EVAL_NAMED 4
typedef int (*asm_eval_fn)(void *ud, const char *begin, const char *end,
                           int64_t *out, char *err, size_t errcap);

/* The kinds of error, which the caller maps to its error numbers. BBC BASIC
 * uses the ARM assembler's errors (see basicasm.c). */
enum {
    ASM_EK_SYNTAX = 0,          /* the operands' form: Syntax error */
    ASM_EK_MNEMONIC,            /* no such instruction: No such mnemonic */
    ASM_EK_REGISTER,            /* a register where it cannot be: Bad register */
    ASM_EK_IMMEDIATE,           /* a value no encoding holds: Bad immediate constant */
    ASM_EK_OFFSET,              /* a target out of reach: Bad address offset */
    ASM_EK_SHIFT,               /* a shift amount: Bad shift */
    ASM_EK_FORM,                /* a mnemonic known, no form for these operands:
                                   Syntax error (BASIC's first look, whose
                                   values are all 0, passes it) */
};

/* Value errors that the encoder assembled past (asm_ctx.soft). The value
 * did not fit, and the instruction holds as much of it as its field does.
 * BBC BASIC reports them as its ARM assembler does. That is only with OPT's
 * errors bit set, in a pass where forward references read as P%. The
 * exception is a branch's offset, which it always reports
 * (ASM_SOFT_ALWAYS). */
#define ASM_SOFT_ALWAYS 0x100

typedef struct asm_ctx {
    asm_eval_fn eval;
    void *ud;
    uint64_t addr;      /* address of the instruction being assembled */
    int addr32;         /* targets are 32-bit addresses: BBC BASIC's
                           integers are signed, &80000000 up read negative */
    char err[192];
    int ekind;          /* the error's kind (ASM_EK_*), when one is returned */
    int soft;           /* 0, or a value error's kind | ASM_SOFT_ALWAYS */
    char softerr[96];   /* ... and its message */
} asm_ctx;

/* A branch's or PC-relative target. With addr32, a negative 32-bit value
 * is the address it spells (&FEEFF000, not 0xFFFFFFFF_FEEFF000). A target
 * out of reach is therefore an error and does not wrap to the top of
 * memory. */
static inline int64_t asm_target(const asm_ctx *c, int64_t v)
{
    return c->addr32 && v < 0 && v >= INT32_MIN ? v + ((int64_t)1 << 32) : v;
}

/* The default evaluator for standalone use. It accepts 0x hex, decimal
 * (octal is not accepted) and negative numbers, with an optional leading
 * '#'. It also accepts "&" hex, which is BBC BASIC's spelling. A bare
 * identifier is reported as unresolved (return 1), so that two-pass
 * assembly can retry once the labels exist. */
int asm_eval_literal(void *ud, const char *begin, const char *end,
                     int64_t *out, char *err, size_t errcap);

#define ASM_OK          0
#define ASM_ESPACE      (-1)   /* output buffer too small */
#define ASM_ESYNTAX     (-2)   /* message in ctx->err, kind in ctx->ekind */

/* Both encoders assemble exactly one instruction from text[0..len), with
 * no newline. On success *outlen bytes were written and the whole text was
 * consumed. */
int asma64_insn(asm_ctx *c, const char *text, size_t len,
                uint8_t *out, size_t cap, size_t *outlen);
int asmx64_insn(asm_ctx *c, const char *text, size_t len,
                uint8_t *out, size_t cap, size_t *outlen);

/* Helpers shared by the encoders. */
int asm__hexval(int ch);
int asm__clideval(asm_ctx *c, const char *b, const char *e, int64_t *out);
/* asm__fail returns an error of a kind (ASM_ESYNTAX). asm__soft records a
 * value error that the encoder assembled past. */
int asm__fail(asm_ctx *c, int kind, const char *fmt, ...);
void asm__soft(asm_ctx *c, int kind, const char *fmt, ...);
/* The end of an item in an operand list. This is the next ',' outside
 * brackets, parentheses and "strings". Examples from BASIC are FNf(a,b),
 * MID$(s$,1,2) and ",". */
const char *asm__item_end(const char *p, const char *e);

#endif
