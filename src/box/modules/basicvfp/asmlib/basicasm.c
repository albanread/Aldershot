/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* basicasm.c: the x86-64 and AArch64 instructions of BBC BASIC's inline
 * assembler. A block of '[' ... ']' after *BasicAsmCPU X64 or A64 uses them.
 *
 * BASIC does everything except encode the instructions. The ROM's ARM
 * assembler (s.Assembler) runs the block for every CPU. It is in the
 * generated BASIC (gen/rom_basicvfp.c, or gen/rom_basicvfp_t3.c with the
 * hand-written interpreter). It handles these things:
 *   statements and lines,
 *   '.label', which makes a BASIC variable holding P%,
 *   OPT and its listing, P%, O% (OPT 4) and L% (OPT 8),
 *   the directives (OPT, EQUB/EQUW/EQUD/EQUS, DCB/DCW/DCD, EQUF/DCF, '=',
 *   '&' and ALIGN),
 *   FN macros, comments and Escape,
 *   every error, which it raises with its number where BASIC raises it.
 * ../patch-basicasm.py puts seven hooks in the ARM assembler.
 *
 *   CASM     This is at the mnemonic, where the ARM assembler looks it up.
 *            A statement that is not a directive comes here
 *            (basicasm_statement). The encoder reads the text. BASIC's own
 *            expression evaluator (EXPR) values each operand that the
 *            encoder asks for. The bytes go back to the ARM assembler's
 *            CASMX. As for an ARM instruction, CASMX places them at P% or
 *            O%, checks L%, lists them and advances P%.
 *   CASMX    This lists x86-64 bytes (basicasm_list_bytes). An A64 word
 *            lists as an ARM word does.
 *   ASS      '[' begins a block (basicasm_block).
 *   CASM1A   '.name' makes a label. The label's variable and value are
 *            noted (basicasm_label).
 *   CASMKET  ']' tells the run path where the block's code is, and the
 *            block ends (basicasm_ket). This happens for every CPU,
 *            including ARM.
 *   TSTVB1   A name that BASIC has no variable for reads as P% (with OPT's
 *            errors bit clear). The hook counts these (basicasm_unknown),
 *            so that the encoder knows the value is not yet the name's own.
 *   MSG      This is BASIC's error handler, which goes on at STMT. An error
 *            in an operand comes back to the statement that it abandons.
 *            That statement then goes on at STMT itself
 *            (basicasm_error_goes_on).
 *
 * The ARM assembler's own rules apply with no extra work. An A64
 * instruction is word-aligned first, as an ARM one is. A value that does
 * not fit is reported only with OPT's errors bit. The exception is a
 * branch out of reach, which BASIC always reports.
 *
 * The x86-64 code must be the same size in every pass, or a label after it
 * would move. A jump, an immediate or a displacement takes the short form
 * only for a value that is settled in this pass. Numbers, P% and labels
 * made earlier in the same '[' are settled. Anything else takes the long
 * form: a name not made yet, any other variable, FN, or indirection. Another
 * pass may see the value of these differently.
 */
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "rosgd/armbox.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"

#include "asm.h"
#include "basicasm.h"

/* The encoders that this build carries (the Makefile's BASICASM). Both are
 * included by default. A build selects one with -DBASICASM_HAVE_X64=0 and
 * the like. */
#ifndef BASICASM_HAVE_X64
#define BASICASM_HAVE_X64 1
#endif
#ifndef BASICASM_HAVE_A64
#define BASICASM_HAVE_A64 1
#endif

/* The CPU that the ROM's BASIC assembles for by default. This is the CPU
 * that it runs on. The Makefile passes it, or else it comes from the
 * compiler's own target. *BasicAsmCPU switches to another CPU, including
 * ARM, for cross assembly. */
#ifndef BASICASM_DEFAULT_CPU
#if defined(__x86_64__) && BASICASM_HAVE_X64
#define BASICASM_DEFAULT_CPU BASICASM_CPU_X64
#elif defined(__aarch64__) && BASICASM_HAVE_A64
#define BASICASM_DEFAULT_CPU BASICASM_CPU_A64
#else
#define BASICASM_DEFAULT_CPU BASICASM_CPU_ARM
#endif
#endif
#if (BASICASM_DEFAULT_CPU == BASICASM_CPU_X64 && !BASICASM_HAVE_X64) || \
    (BASICASM_DEFAULT_CPU == BASICASM_CPU_A64 && !BASICASM_HAVE_A64)
#undef BASICASM_DEFAULT_CPU                 /* the encoder for it is not in this build */
#define BASICASM_DEFAULT_CPU BASICASM_CPU_ARM
#endif

/* The CPU whose code runs on this machine. CALL and USR enter that code. */
#if defined(__x86_64__)
#define BASICASM_NATIVE_CPU BASICASM_CPU_X64
#elif defined(__aarch64__)
#define BASICASM_NATIVE_CPU BASICASM_CPU_A64
#else
#define BASICASM_NATIVE_CPU (-1)
#endif

/* The CPU chosen by *BasicAsmCPU. Each task has its own, as it does for a
 * thread. A new task starts with the machine's CPU. */
_Thread_local int basicasm_cpu = BASICASM_DEFAULT_CPU;
_Thread_local unsigned basicasm_unknown;

int basicasm_cpu_mode(void)
{
    return basicasm_cpu;
}

static _Thread_local int arm_by_recognition;
static _Thread_local int cpu_chosen;            /* set by *BasicAsmCPU <cpu>: no recognition then */

void basicasm_set_cpu(int cpu)
{
    basicasm_cpu = cpu;
    arm_by_recognition = 0;                         /* the CPU was chosen and not recognised */
    cpu_chosen = 1;
}

/* *BasicAsmCPU with no CPU. This selects the machine's CPU and turns ARM
 * program recognition on again. */
void basicasm_default(void)
{
    basicasm_cpu = BASICASM_DEFAULT_CPU;
    arm_by_recognition = 0;
    cpu_chosen = 0;
}

/* ARM by recognition. A RISC OS program written for an ARM machine
 * assembles ARM code and does not say so. Its '[' blocks are ARM's. This
 * applies while the task is on the machine's own CPU and no *BasicAsmCPU
 * has chosen one. The native encoder may refuse a statement. If that
 * statement reads as ARM, the task's assembler turns to ARM from that
 * statement on. A statement reads as ARM if it has:
 *   a register R0-R15,
 *   an immediate '#',
 *   a register list '{',
 *   LDM, STM or SWI,
 *   or an ARM mnemonic that the encoder does not know at all (x86-64 has
 *   no B).
 * RISC OS's own ARM assembler then takes the statement. The block's code
 * is ARM code that the ARM container runs (basicasm_ket). ROSGD's own
 * programs choose their CPU, or they write the machine's own registers
 * (x0, w9, eax), which the native encoder takes. The task stays on ARM, so
 * the passes after the first assemble ARM throughout.
 *
 * arm_by_recognition, above, records that ARM was selected this way and not
 * by *BasicAsmCPU. */
static _Thread_local uint32_t arm_program;     /* the program it applies to, as a fingerprint */

/* A fingerprint of the program in memory, made from PAGE, TOP and the first
 * bytes of the program. Another program may run in the same task, such as a
 * TaskWindow's next command. It is recognised afresh, starting from the
 * machine's CPU. */
static uint32_t program_fingerprint(uint32_t argp)
{
    uint32_t page = ros_ld32(argp - 148), top = ros_ld32(argp - 144), h = 2166136261u;
    h = (h ^ page) * 16777619u;
    h = (h ^ top) * 16777619u;
    for (uint32_t a = page; a < page + 64 && a < top; a++)
        h = (h ^ ros_ld8(a)) * 16777619u;
    return h;
}

/* Says whether the word is an ARM mnemonic. A condition and suffixes are
 * allowed. B and BL are matched whole, with a condition (B, BLT, BLNE and so
 * on). The others are matched by their first three letters. */
static int arm_mnemonic(const char *w)
{
    static const char *const cond[] = { "", "eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs",
                                        "vc", "hi", "ls", "ge", "lt", "gt", "le", "al" };
    static const char *const three[] = { "mov", "mvn", "add", "adc", "sub", "sbc", "rsb", "rsc",
                                         "and", "orr", "eor", "bic", "cmp", "cmn", "tst", "teq",
                                         "mul", "mla", "ldr", "str", "ldm", "stm", "swi", "swp",
                                         "adr", "mrs", "msr" };
    for (unsigned k = 0; k < sizeof cond / sizeof cond[0]; k++) {
        char b[8];
        snprintf(b, sizeof b, "b%s", cond[k]);
        if (!strcmp(w, b))
            return 1;
        snprintf(b, sizeof b, "bl%s", cond[k]);
        if (!strcmp(w, b))
            return 1;
    }
    for (unsigned k = 0; k < sizeof three / sizeof three[0]; k++)
        if (!strncmp(w, three[k], 3))
            return 1;
    return 0;
}

static int reads_as_arm(const char *t, size_t n, int unknown_mnemonic)
{
    char w[8];
    size_t k = 0;
    while (k < n && k < sizeof w - 1 && isalpha((unsigned char)t[k]))
        w[k] = (char)tolower((unsigned char)t[k]), k++;
    w[k] = 0;
    if (!strncmp(w, "ldm", 3) || !strncmp(w, "stm", 3) || !strncmp(w, "swi", 3))
        return 1;
    if (unknown_mnemonic && arm_mnemonic(w))      /* "b skip" to the x86-64 encoder */
        return 1;
    for (size_t i = k; i < n; i++) {
        char c = t[i];
        if (c == '#' || c == '{')
            return 1;
        if ((c == 'r' || c == 'R') && (i == 0 || !isalnum((unsigned char)t[i - 1])) && i + 1 < n &&
            isdigit((unsigned char)t[i + 1])) {
            size_t j = i + 1;
            unsigned v = 0;
            while (j < n && isdigit((unsigned char)t[j]))
                v = v * 10 + (unsigned)(t[j++] - '0');
            if (v <= 15 && (j == n || !isalnum((unsigned char)t[j])))
                return 1;
        }
    }
    return 0;
}

int basicasm_default_cpu(void)
{
    return BASICASM_DEFAULT_CPU;
}

/* BASIC's own evaluator. patch-basicasm.py exports it from the ROM. */
void basicvfp_EXPR(struct ros_cpu *s);

/* ---- BASIC's workspace (hdr/WorkSpace), at offsets from ARGP ---- */

#define WS_STRACC   (-0x600)        /* the string accumulator */
#define WS_PROCPTR  (-0x200)        /* the variable lists, 4*(c-'@') on */
#define WS_INTVAR   (-0x100)        /* @%, A%-Z% */
#define WS_P        (-192)          /* P% (ASSPC) */
#define WS_O        (-196)          /* O% */
#define WS_BYTESM   (-26)           /* OPT */
#define WS_FSA      (-140)          /* the free space's start */
#define OPT_ERRORS  2u
#define OPT_TO_O    4u
/* The variable cache (VCACHE at ARGP+0, in BASICVFP's FPOINT=2 layout).
 * Its entries are 16 bytes each. An entry is found by the bottom 8 bits of
 * a name's address in the program. The name's address is at +8
 * (CACHECHECK). */
#define CACHE_MASK  0xFFu
#define CACHE_SHIFT 4
#define CACHE_CHECK 8

#define TCONST 0x8Du                /* a line number: three bytes follow */
#define TFN    0xA4u
#define TPROC  0xF2u
#define TREM   0xF4u

/* ---- BASIC's keywords, so that the encoders get the text as written ---- */

static const char *const tok1[0x81] = {     /* &7F-&FF */
    "OTHERWISE", "AND", "DIV", "EOR", "MOD", "OR", "ERROR", "LINE", "OFF",
    "STEP", "SPC", "TAB(", "ELSE", "THEN", NULL, "OPENIN", "PTR", "PAGE",
    "TIME", "LOMEM", "HIMEM", "ABS", "ACS", "ADVAL", "ASC", "ASN", "ATN",
    "BGET", "COS", "COUNT", "DEG", "ERL", "ERR", "EVAL", "EXP", "EXT",
    "FALSE", "FN", "GET", "INKEY", "INSTR(", "INT", "LEN", "LN", "LOG",
    "NOT", "OPENUP", "OPENOUT", "PI", "POINT(", "POS", "RAD", "RND", "SGN",
    "SIN", "SQR", "TAN", "TO", "TRUE", "USR", "VAL", "VPOS", "CHR$", "GET$",
    "INKEY$", "LEFT$(", "MID$(", "RIGHT$(", "STR$", "STRING$(", "EOF",
    NULL, NULL, NULL, "WHEN", "OF", "ENDCASE", "ELSE", "ENDIF", "ENDWHILE",
    "PTR", "PAGE", "TIME", "LOMEM", "HIMEM", "SOUND", "BPUT", "CALL",
    "CHAIN", "CLEAR", "CLOSE", "CLG", "CLS", "DATA", "DEF", "DIM", "DRAW",
    "END", "ENDPROC", "ENVELOPE", "FOR", "GOSUB", "GOTO", "GCOL", "IF",
    "INPUT", "LET", "LOCAL", "MODE", "MOVE", "NEXT", "ON", "VDU", "PLOT",
    "PRINT", "PROC", "READ", "REM", "REPEAT", "REPORT", "RESTORE", "RETURN",
    "RUN", "STOP", "COLOUR", "TRACE", "UNTIL", "WIDTH", "OSCLI",
};
static const char *const tokfn[] = { "SUM", "BEAT" };                 /* &C6 &8E.. */
static const char *const tokcmd[] = {                                 /* &C7 &8E.. */
    "APPEND", "AUTO", "CRUNCH", "DELETE", "EDIT", "HELP", "LIST", "LOAD",
    "LVAR", "NEW", "OLD", "RENUMBER", "SAVE", "TEXTLOAD", "TEXTSAVE",
    "TWIN", "TWINO", "INSTALL",
};
static const char *const tokstmt[] = {                                /* &C8 &8E.. */
    "CASE", "CIRCLE", "FILL", "ORIGIN", "POINT", "RECTANGLE", "SWAP",
    "WHILE", "WAIT", "MOUSE", "QUIT", "SYS", "INSTALL", "LIBRARY", "TINT",
    "ELLIPSE", "BEATS", "TEMPO", "VOICES", "VOICE", "STEREO", "OVERLAY",
};

/* The keyword that the token at p stands for, and the token's length in
 * bytes. */
static const char *token(uint32_t p, uint32_t *len)
{
    unsigned t = ros_ld8(p), t2 = ros_ld8(p + 1);
    *len = 1;
#define TWO(tab)                                                              \
    do {                                                                      \
        *len = 2;                                                             \
        return t2 >= 0x8E && t2 - 0x8E < sizeof tab / sizeof tab[0] ? tab[t2 - 0x8E] : "?"; \
    } while (0)
    if (t == 0xC6)
        TWO(tokfn);
    if (t == 0xC7)
        TWO(tokcmd);
    if (t == 0xC8)
        TWO(tokstmt);
#undef TWO
    if (t == TCONST) {
        *len = 4;
        return NULL;
    }
    return tok1[t - 0x7F] ? tok1[t - 0x7F] : "?";
}

/* ---- one statement ---- */

#define STMT_MAX 1024

typedef struct {
    struct ros_cpu *s;
    uint32_t here;              /* where the instruction goes (P%) */
    asm_ctx *c;                 /* the encoder's, its addr kept to P% */
    int err;                    /* a BASIC error an operand raised */
    size_t n;
    char buf[STMT_MAX];         /* the text the encoder reads: keywords
                                   spelt out, lower case but for strings */
    uint32_t at[STMT_MAX + 1];  /* buf[i] came from at[i]; at[n] the end */
} stmt;

/* The end of a statement, as the ARM assembler's ASMCHK finds it. It is
 * a ':' or CR, or the start of a comment (';', '\' or REM). Characters
 * inside strings do not count. */
static uint32_t stmt_end(uint32_t p)
{
    int quote = 0;
    for (;; p++) {
        unsigned ch = ros_ld8(p);
        if (ch == 13)
            return p;
        if (quote) {
            quote = ch != '"';
            continue;
        }
        if (ch == '"')
            quote = 1;
        else if (ch == ':' || ch == ';' || ch == '\\' || ch == TREM)
            return p;
        else if (ch == 0xC6 || ch == 0xC7 || ch == 0xC8)
            p++;
        else if (ch == TCONST)
            p += 3;
    }
}

static void put(stmt *t, char ch, uint32_t from)
{
    if (t->n < STMT_MAX) {
        t->buf[t->n] = ch;
        t->at[t->n++] = from;
    }
}

/* Writes [p, e) as the encoder reads it. Keywords are spelt out and put in
 * lower case. BASIC's keywords are upper case, and MC's mnemonics and
 * registers are accepted in any case. Strings are left as they are. at[]
 * maps each character back to its place in the program. An operand is
 * therefore valued from the text that BASIC tokenised. */
static int spell(stmt *t, uint32_t p, uint32_t e)
{
    int quote = 0;
    t->n = 0;
    while (p < e) {
        unsigned ch = ros_ld8(p);
        if (quote || ch == '"') {
            if (ch == '"')
                quote = !quote;
            put(t, (char)ch, p++);
            continue;
        }
        if (ch >= 0x7F) {
            uint32_t len;
            const char *k = token(p, &len);
            if (!k) {                   /* a line number, which belongs to GOTO and the like */
                char num[8];
                unsigned b1 = ros_ld8(p + 1), b2 = ros_ld8(p + 2), b3 = ros_ld8(p + 3);
                unsigned n = ((b1 << 2) & 0xC0) ^ b2;
                n |= (((b1 << 4) ^ b3) & 0xFF) << 8;
                snprintf(num, sizeof num, "%u", n);
                for (const char *q = num; *q; q++)
                    put(t, *q, p);
            } else
                for (const char *q = k; *q; q++)
                    put(t, (char)tolower((unsigned char)*q), p);
            p += len;
            continue;
        }
        put(t, (char)tolower((int)ch), p++);
    }
    t->at[t->n] = e;
    return t->n < STMT_MAX;
}

/* ---- blocks and their labels: what is settled in this pass ---- */

/* Each '[' starts a block with a serial number of its own, which is never
 * used again. A block can be inside another one. This happens with an FN
 * called from an operand or as a macro. The inner block is on top of the
 * outer one until its ']'. An error can leave a block without ending it.
 * The next '[' still gets a new serial number, and that is all that
 * matters. */
#define BLOCKS_DEEP 16
static _Thread_local unsigned blk_stack[BLOCKS_DEEP];
static _Thread_local int blk_depth;
static _Thread_local unsigned blk_next;

static unsigned block_now(void)
{
    return blk_depth ? blk_stack[blk_depth - 1] : 0;
}

void basicasm_block(uint32_t argp)
{
    if (arm_by_recognition && program_fingerprint(argp) != arm_program) {
        arm_by_recognition = 0;                     /* another program: recognised afresh */
        basicasm_cpu = BASICASM_DEFAULT_CPU;
    }
    if (blk_depth == BLOCKS_DEEP) {
        memmove(blk_stack, blk_stack + 1, sizeof blk_stack - sizeof blk_stack[0]);
        blk_depth--;
    }
    if (++blk_next == 0)
        blk_next = 1;
    blk_stack[blk_depth++] = blk_next;
}

static void block_end(void)
{
    if (blk_depth)
        blk_depth--;
}

/* The labels made in the current block. For each one the table holds its
 * variable (the address of its value, as BASIC's LV gives it) and the P%
 * that it was given. A name that is one of these, and still has that
 * value, is settled. The code before it is the same size in every pass, so
 * the label has the same value in every pass. A label that the table has no
 * room for is never recorded, so it counts as not settled in every pass. */
#define LABELS 512
#define LABEL_PROBES 8
static _Thread_local struct label_ {
    uint32_t var, value;
    unsigned block;
} labels[LABELS];

static unsigned label_slot(uint32_t var, int k)
{
    return ((var >> 2) * 2654435761u + (unsigned)k) % LABELS;
}

void basicasm_label(uint32_t var, uint32_t value)
{
    unsigned now = block_now();
    if (!now)
        return;
    for (int k = 0; k < LABEL_PROBES; k++) {
        struct label_ *l = &labels[label_slot(var, k)];
        if (l->block != now || l->var == var) {
            l->var = var, l->value = value, l->block = now;
            return;
        }
    }
}

static int label_settled(uint32_t var, uint32_t value)
{
    unsigned now = block_now();
    if (!now)
        return 0;
    for (int k = 0; k < LABEL_PROBES; k++) {
        const struct label_ *l = &labels[label_slot(var, k)];
        if (l->block == now && l->var == var)
            return l->value == value;
    }
    return 0;
}

/* ---- BASIC's variables ---- */

/* Finds a variable of BASIC's in the way that BASIC's own LV finds it. See
 * s/Expr and LVCONT, and CREATE for the layout, and hdr/WorkSpace.
 * @% and A%-Z% are the resident integers. They are words at INTVAR
 * (ARGP-&100). Any other variable is on the list for its first character.
 * The lists start at PROCPTR (ARGP-&200), at 4*(c-'@'). Each entry holds:
 *   a link word,
 *   the name after its first character, with its type suffix,
 *   a zero,
 *   the value, word-aligned. This holds for an integer and for BASICVFP's
 *   8-byte double.
 * Names are case-sensitive, as BASIC's are.
 * [b, e) is the name, including its suffix. The function sets *at to the
 * address of the value and *v to the value as an integer. It returns 1 if
 * the variable is found and 0 if it is not. It also returns 0 for a
 * string. */
static int basic_var(uint32_t argp, const char *b, const char *e, uint32_t *at, int64_t *v)
{
    size_t len = (size_t)(e - b);
    unsigned char c = (unsigned char)b[0];
    if (!argp || len == 0 || c < '@' || c > 'z')
        return 0;
    char suffix = e[-1];
    if (suffix == '$')
        return 0;                       /* strings: not a number */
    if (len == 2 && suffix == '%' && c <= 'Z') {
        *at = argp + WS_INTVAR + 4 * (c - '@');
        *v = (int32_t)ros_ld32(*at);
        return 1;
    }
    size_t rest = len - 1;
    for (uint32_t entry = ros_ld32(argp + WS_PROCPTR + 4 * (c - '@')); entry;
         entry = ros_ld32(entry)) {
        uint32_t name = entry + 4;
        size_t i = 0;
        while (i < rest && ros_ld8(name + i) == (unsigned char)b[1 + i])
            i++;
        if (i < rest || ros_ld8(name + i) != 0)
            continue;
        uint32_t value = (name + i + 1 + 3) & ~3u;
        *at = value;
        if (suffix == '%') {
            *v = (int32_t)ros_ld32(value);
        } else {
            double d;
            memcpy(&d, ros_ptr(value), sizeof d);
            *v = isfinite(d) && fabs(d) < 9.2e18 ? (int64_t)d : 0;
        }
        return 1;
    }
    return 0;
}

static int namech(unsigned ch)
{
    return isalnum(ch) || ch == '_' || ch == '`';
}

/* Says whether a token is one of BASIC's functions or operators whose value
 * depends only on its arguments (tokens &80-&C4, see tok1). Anything else
 * may differ from pass to pass. That includes FN, TIME, RND, EVAL, PAGE and
 * a stream. */
static int pure_token(unsigned t)
{
    static const unsigned char pure[] = {
        0x80, 0x81, 0x82, 0x83, 0x84,                   /* AND DIV EOR MOD OR */
        0x94, 0x95, 0x97, 0x98, 0x99, 0x9B, 0x9D,       /* ABS ACS ASC ASN ATN COS DEG */
        0xA1, 0xA3, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, /* EXP FALSE INSTR( INT LEN LN LOG NOT */
        0xAF, 0xB2, 0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBB, /* PI RAD SGN SIN SQR TAN TRUE VAL */
        0xBD, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4,             /* CHR$ LEFT$( MID$( RIGHT$( STR$ STRING$( */
    };
    return memchr(pure, (int)t, sizeof pure) != NULL;
}

/* What the tokenised expression [p, e) depends on.
 * DEP_NAMED is set if the expression names anything, such as a variable, a
 * function or an indirection. It is then not a plain number.
 * DEP_UNSETTLED is set if the value may be different in another pass. This
 * is the case for any name except P% and a label that was made earlier in
 * this block and still has that value. */
#define DEP_NAMED 1
#define DEP_UNSETTLED 2
static int depends(uint32_t argp, uint32_t p, uint32_t e)
{
    int dep = 0;
    while (p < e) {
        unsigned ch = ros_ld8(p);
        if (ch == '"') {
            for (p++; p < e && ros_ld8(p) != '"'; p++)
                ;
            p++;
        } else if (ch == '&') {
            for (p++; p < e && isxdigit(ros_ld8(p)); p++)
                ;
        } else if (ch == '%') {
            for (p++; p < e && (ros_ld8(p) == '0' || ros_ld8(p) == '1'); p++)
                ;
        } else if (ch == TFN || ch == TPROC) {
            dep |= DEP_NAMED | DEP_UNSETTLED;
            for (p++; p < e && namech(ros_ld8(p)); p++)
                ;
        } else if (ch == TCONST) {
            p += 4;                     /* a line number */
        } else if (ch >= 0x7F) {
            if (ch == 0xC6 || ch == 0xC7 || ch == 0xC8 || !pure_token(ch))
                dep |= DEP_NAMED | DEP_UNSETTLED;
            p += ch == 0xC6 || ch == 0xC7 || ch == 0xC8 ? 2 : 1;
        } else if (isdigit(ch) || ch == '.') {
            for (p++; p < e && (isdigit(ros_ld8(p)) || ros_ld8(p) == '.'); p++)
                ;
            if (p < e && ros_ld8(p) == 'E') {
                p++;
                if (p < e && (ros_ld8(p) == '-' || ros_ld8(p) == '+'))
                    p++;
                while (p < e && isdigit(ros_ld8(p)))
                    p++;
            }
        } else if (isalpha(ch) || ch == '_' || ch == '`' || ch == '@') {
            uint32_t b = p++;
            while (p < e && namech(ros_ld8(p)))
                p++;
            if (p < e && (ros_ld8(p) == '%' || ros_ld8(p) == '$'))
                p++;
            dep |= DEP_NAMED;
            uint32_t at;
            int64_t v;
            if (p - b == 2 && ros_ld8(b) == 'P' && ros_ld8(b + 1) == '%')
                continue;               /* P%: the instruction's address */
            if (p < e && ros_ld8(p) == '(')
                dep |= DEP_UNSETTLED;   /* an array's element */
            else if (!basic_var(argp, (const char *)ros_ptr(b), (const char *)ros_ptr(p), &at,
                                &v) || !label_settled(at, (uint32_t)v))
                dep |= DEP_UNSETTLED;
        } else if (ch == '!' || ch == '?' || ch == '$' || ch == '|') {
            dep |= DEP_NAMED | DEP_UNSETTLED;   /* indirection */
            p++;
        } else
            p++;
    }
    return dep;
}

/* ---- valuing an operand: BASIC's EXPR ---- */

/* Forgets the variable cache's entries for [lo, hi]. EXPR caches a name by
 * its address in the text. The text here is a copy on BASIC's stack, as
 * EVAL's is (s.Factor EVAL, PURGECACHE). */
static void purge_cache(uint32_t argp, uint32_t lo, uint32_t hi)
{
    for (uint32_t a = lo; a <= hi; a++) {
        uint32_t entry = argp + ((a & CACHE_MASK) << CACHE_SHIFT);
        if (ros_ld32(entry + CACHE_CHECK) == a)
            ros_st32(entry + CACHE_CHECK, 0);
    }
}

/* Says whether the text is a number that MC's syntax writes, which BASIC
 * does not read or holds only as a real. It is decimal or 0x hex, with or
 * without a sign. It is valued as MC values it, in 64 bits. */
static int mc_literal(const char *b, const char *e)
{
    if (b < e && (*b == '-' || *b == '+'))
        b++;
    while (b < e && isspace((unsigned char)*b))
        b++;
    if (b == e)
        return 0;
    if (e - b > 2 && b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) {
        for (b += 2; b < e; b++)
            if (!isxdigit((unsigned char)*b))
                return 0;
        return 1;
    }
    for (; b < e; b++)
        if (!isdigit((unsigned char)*b))
            return 0;
    return 1;
}

/* The eval callback for the encoders (see asm.h). It values the operand
 * [b, e) of t->buf in the way that the ARM assembler values one
 * (s.Assembler ASMEXPR). EXPR evaluates it, a string is refused, and a
 * real is made an integer. It works on a copy of the text that BASIC
 * tokenised, as EVAL evaluates its string. An error in the operand is
 * BASIC's. EXPR raises it and does not come back here. A real outside 32
 * bits is kept whole. This allows the 64-bit immediates that BASIC's
 * integers cannot hold. depends() decides whether the value is settled (0)
 * or not (2), and whether it is named (ASM_EVAL_NAMED). */
static int eval_cb(void *ud, const char *b, const char *e, int64_t *out, char *err,
                   size_t errcap)
{
    stmt *t = ud;
    struct ros_cpu *s = t->s;
    while (b < e && isspace((unsigned char)*b))
        b++;
    while (e > b && isspace((unsigned char)e[-1]))
        e--;
    if (b == e) {
        t->err = BASICASM_FACERR;       /* EXPR's, finding nothing */
        snprintf(err, errcap, "missing value");
        return -1;
    }
    if (mc_literal(b, e)) {
        int r = asm_eval_literal(NULL, b, e, out, err, errcap);
        if (r < 0)
            t->err = BASICASM_ERASS2;   /* more than 64 bits */
        return r;
    }

    uint32_t ob = t->at[b - t->buf], oe = t->at[e - t->buf];
    uint32_t len = oe - ob;
    if (oe < ob || len > 250) {
        snprintf(err, errcap, "operand too long");
        return -1;
    }
    /* A character that cannot begin an expression is junk after the
     * instruction, as the ARM assembler's ASMCHK finds it ("nop ]"). It is
     * a syntax error. EXPR would make something else of it. */
    unsigned first = ros_ld8(ob);
    if (!(isalnum(first) || first >= 0x7F || strchr("&%.\"(-+!?$|@`_", (int)first))) {
        snprintf(err, errcap, "junk in operand");
        return -1;
    }
    uint32_t argp = s->r[8];
    uint32_t saved[16];
    uint32_t n = s->n, z = s->z, c = s->c, v = s->v;
    memcpy(saved, s->r, sizeof saved);
    uint32_t text = (s->r[13] - 256) & ~3u;     /* below BASIC's sp, as EVAL's */
    for (uint32_t i = 0; i < len; i++)
        ros_st8(text + i, ros_ld8(ob + i));
    ros_st8(text + len, 13);
    purge_cache(argp, text, text + len);
    unsigned unknown = basicasm_unknown;
    s->r[11] = text;
    s->r[13] = text;
    s->r[14] = ROS_RETURN_TO_NATIVE;
    basicvfp_EXPR(s);
    if (s->r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(s, ROS_RETURN_TO_NATIVE);
    uint32_t type = s->r[9], iacc = s->r[0], next = s->r[10];
    double facc = s->fp->vfp.d[0];
    purge_cache(argp, text, text + len);
    int forward = basicasm_unknown != unknown;
    memcpy(s->r, saved, sizeof saved);
    s->n = n, s->z = z, s->c = c, s->v = v;
    /* An FN in the operand may have moved P%. The instruction goes where P%
     * is now, as the ARM assembler's does, so it is encoded for that
     * address. */
    uint32_t p = ros_ld32(argp + WS_P);
    if (p != t->here && t->c) {
        t->here = p;
        t->c->addr = p;
    }
    if (next != 13) {                   /* more than one expression */
        snprintf(err, errcap, "junk in operand");
        return -1;
    }
    if (type == 0) {                    /* a string */
        t->err = BASICASM_ERTYPEINT;
        snprintf(err, errcap, "a number is needed");
        return -1;
    }
    if (type & 0x80000000u) {           /* a real, rounded towards 0 */
        if (!isfinite(facc) || fabs(facc) >= 9.2e18) {
            t->err = BASICASM_EINVOP;       /* as FIX raises it for ARM's */
            snprintf(err, errcap, "number too big");
            return -1;
        }
        *out = (int64_t)facc;
    } else
        *out = (int32_t)iacc;
    int dep = depends(argp, ob, oe);
    if (dep & DEP_UNSETTLED)
        forward = 1;
    return (forward ? 2 : 0) | (dep & DEP_NAMED ? ASM_EVAL_NAMED : 0);
}

/* The first look at a statement. Every operand is 0 and BASIC is not
 * asked. The question is whether the encoder knows the mnemonic. The ARM
 * assembler looks up its mnemonic before it reads an operand. So "No such
 * mnemonic" comes before any error that an operand would raise, and before
 * the alignment. A form that the zeros do not fit is ASM_EK_FORM, because
 * the mnemonic is known. An example is AArch64's "ld1 {v0.16b}, [x0], #16". */
static int dry_cb(void *ud, const char *b, const char *e, int64_t *out, char *err,
                  size_t errcap)
{
    (void)ud, (void)b, (void)e, (void)err, (void)errcap;
    *out = 0;
    return 0;
}

static int encode(asm_ctx *c, const char *text, size_t len, uint8_t *out, size_t cap,
                  size_t *n)
{
#if BASICASM_HAVE_X64 && BASICASM_HAVE_A64
    return basicasm_cpu == BASICASM_CPU_X64 ? asmx64_insn(c, text, len, out, cap, n)
                                            : asma64_insn(c, text, len, out, cap, n);
#elif BASICASM_HAVE_X64
    return asmx64_insn(c, text, len, out, cap, n);
#elif BASICASM_HAVE_A64
    return asma64_insn(c, text, len, out, cap, n);
#else
    (void)text, (void)len, (void)out, (void)cap, (void)n;
    c->ekind = ASM_EK_MNEMONIC;         /* no encoder in this build */
    return ASM_ESYNTAX;
#endif
}

/* ---- the statement ---- */

/* Says whether the statement at p is a directive. These are the ARM
 * assembler's own, which it finds in the same way (s.Assembler, CASMTB). It
 * compares the first three letters, in any case. */
static int directive(uint32_t p)
{
    static const char *const dir[] = { "OPT", "EQU", "DCB", "DCW", "DCD", "DCF", "ALI" };
    char w[3];
    for (int i = 0; i < 3; i++)
        w[i] = (char)toupper(ros_ld8(p + (uint32_t)i));
    for (unsigned i = 0; i < sizeof dir / sizeof dir[0]; i++)
        if (!memcmp(w, dir[i], 3))
            return 1;
    return 0;
}

static int error_for(int kind)
{
    switch (kind) {
    case ASM_EK_MNEMONIC:
        return BASICASM_ERASS1;
    case ASM_EK_REGISTER:
        return BASICASM_ERASS3;
    case ASM_EK_IMMEDIATE:
        return BASICASM_ERASS2;
    case ASM_EK_OFFSET:
        return BASICASM_ERASS2A;
    case ASM_EK_SHIFT:
        return BASICASM_ERASS2S;
    case ASM_EK_FORM:                   /* the mnemonic is known, but not with these operands */
    default:
        return BASICASM_ERSYNT;
    }
}

/* The x86-64 bytes that are waiting for CASMX's listing. They are keyed by
 * the statement (LINE) and the place they went (P%). Each task has its own,
 * as it does for a thread. */
static _Thread_local struct {
    uint32_t line, p;
    size_t n;
    uint8_t b[16];
} pending;

static int statement(struct ros_cpu *s, stmt *t, uint32_t start, uint32_t end);

/* An error that BASIC raises while an operand is valued goes on at BASIC's
 * handler. The error may be EXPR's own, or it may come from an FN that the
 * operand calls. s.Basic's MSG ends by loading SP from ERRSTK and
 * branching to STMT, from wherever it was. For the ROM's own code, this
 * leaves the C frames between that place and the handler on the thread's
 * stack. A statement's frames (the encoder's and this driver's) would add a
 * few KB with each error.
 *
 * So each statement is a catch point. The handler's stack (ERRSTK) may be
 * at or above a statement's stack. That means the handler is outside the
 * statement. MSG (patched, basicasm_error_goes_on) then comes back to the
 * outermost such statement, and BASIC goes on at STMT from there, with the
 * frames gone. An FN's own handler (LOCAL ERROR in it, deeper) goes on where
 * it is.
 *
 * This applies only to a statement of the same BASIC, meaning the same
 * struct ros_cpu. An error raised by a SWI reaches BASIC's handler from the
 * runtime's own error path (environment.c), on a CPU of its own. BASIC goes
 * on from there as it does after any SWI error, and the statement's frames
 * are left as they are.
 *
 * A longjmp can leave a statement's frame. This happens when the program
 * ends in an FN (END or QUIT), or when an error unwinds to its base. The
 * longjmp drops that statement's catch point (basicasm_unwind_below, called
 * from dispatch.c's ros_resume_unwind). */
struct catch_ {
    jmp_buf jb;
    uint32_t sp;
    struct ros_cpu *s;
    struct catch_ *prev;
};
static _Thread_local struct catch_ *catch_top;

void basicasm_error_goes_on(struct ros_cpu *s)
{
    struct catch_ *to = NULL;
    for (struct catch_ *k = catch_top; k && k->s == s && k->sp <= s->r[13]; k = k->prev)
        to = k;
    if (!to)
        return;
    catch_top = to->prev;
    ros_resume_unwind(to);
    longjmp(to->jb, 1);
}

/* Called for a longjmp to the frame `to`. The statements below it are gone.
 * The stack grows down, and the catch points are in their frames. */
void basicasm_unwind_below(const void *to)
{
    while (catch_top && (uintptr_t)catch_top < (uintptr_t)to)
        catch_top = catch_top->prev;
}

int basicasm_statement(struct ros_cpu *s)
{
    uint32_t argp = s->r[8];
    uint32_t start = s->r[11] - 1;      /* R10 holds the statement's first character, and R11 is past it */
    if (directive(start))
        return BASICASM_ARM;
    uint32_t end = stmt_end(start);

    /* The statement's text goes on BASIC's stack, below its sp, and EXPR's
     * work area is below that. An error in an operand is BASIC's. BASIC goes
     * on at its handler and does not come back here. It resets that stack,
     * where C's stack would keep the frame. The ROM's own frames stay, and
     * they are small. The room is as much as EVAL wants. */
    uint32_t sp = s->r[13];
    uint32_t area = (sp - (uint32_t)sizeof(stmt)) & ~15u;
    if (area < ros_ld32(argp + WS_FSA) + 1024 + 512)
        return BASICASM_ERDEEPNEST;
    stmt *t = (stmt *)ros_ptr(area);
    struct catch_ k;
    k.sp = sp;
    k.s = s;
    k.prev = catch_top;
    if (setjmp(k.jb))
        return BASICASM_STMT;           /* the error's handler, using MSG's registers */
    catch_top = &k;
    s->r[13] = area;
    int rc = statement(s, t, start, end);
    s->r[13] = sp;
    catch_top = k.prev;
    return rc;
}

/* Says whether an operand is missing after a comma ("add eax," or
 * "add w0,w1,,x"). The ARM assembler asks EXPR for it, and EXPR finds
 * nothing ("Unknown or missing variable"). The encoders would see a form
 * that they do not know. */
static int missing_operand(const stmt *t)
{
    int quoted = 0;
    for (size_t i = 0; i < t->n; i++) {
        if (t->buf[i] == '"')
            quoted = !quoted;
        if (quoted || t->buf[i] != ',')
            continue;
        size_t j = i + 1;
        while (j < t->n && isspace((unsigned char)t->buf[j]))
            j++;
        if (j == t->n || t->buf[j] == ',')
            return 1;
    }
    return 0;
}

/* ARM is recognised by reading a block ahead (#178). A statement that the
 * native encoder takes is native, even if it is also ARM. x86-64 has
 * R8-R15 and takes an immediate's '#'. So AWViewer's first instruction,
 * "cmp r11,#(TableEnd-TableStart)DIV4", is good x86-64.
 *
 * Suppose ARM were recognised only at the next statement. The block's first
 * pass would have seven x86-64 bytes for that instruction where ARM has
 * four, and every label after it would be four bytes out. The passes after
 * that assemble ARM throughout. They take the first pass's values for the
 * labels ahead of them, and each forward branch lands a word past its label.
 * In AWViewer, the callback lands a word past its STMFD and returns to 0.
 *
 * So at the first statement of a block, the block is read through to its
 * ']'. Each statement is looked at in the way that statement() looks at
 * one. If one reads as ARM, the block is ARM from its start. */
#define LOOKAHEAD_MAX 4096          /* statements; a longer block is read only this far */
static _Thread_local unsigned looked_at;    /* the serial of the block read ahead */

static int block_reads_as_arm(stmt *t, uint32_t argp, uint32_t p)
{
    for (unsigned k = 0; k < LOOKAHEAD_MAX; k++) {
        unsigned ch = ros_ld8(p);
        while (ch == ' ' || ch == '\t')
            ch = ros_ld8(++p);
        if (ch == 13) {
            if (ros_ld8(p + 1) & 0x80)          /* the program's end */
                return 0;
            p += 4;                             /* CR, the line number, its length */
            continue;
        }
        if (ch == ']')
            return 0;
        if (ch == ':') {
            p++;
            continue;
        }
        if (ch == ';' || ch == '\\' || ch == TREM) {    /* a comment: to the line's end */
            while (ros_ld8(p) != 13)
                p++;
            continue;
        }
        if (ch == '.') {                        /* a label */
            p++;
            for (ch = ros_ld8(p); isalnum(ch) || ch == '_' || ch == '`' || ch == '%' || ch == '$';
                 ch = ros_ld8(++p))
                ;
            continue;
        }
        uint32_t e = stmt_end(p);
        if (e == p) {
            p++;
            continue;
        }
        if (!directive(p) && spell(t, p, e)) {
            uint8_t out[32];
            size_t n = 0;
            asm_ctx c;
            memset(&c, 0, sizeof c);
            c.eval = dry_cb;
            c.addr = ros_ld32(argp + WS_P);
            if (encode(&c, t->buf, t->n, out, sizeof out, &n) != ASM_OK &&
                reads_as_arm(t->buf, t->n, c.ekind == ASM_EK_MNEMONIC))
                return 1;
        }
        p = e;
    }
    return 0;
}

static int statement(struct ros_cpu *s, stmt *t, uint32_t start, uint32_t end)
{
    uint32_t argp = s->r[8];
    t->s = s;
    t->err = 0;
    if (!cpu_chosen && basicasm_cpu == BASICASM_DEFAULT_CPU && looked_at != block_now()) {
        looked_at = block_now();
        if (block_reads_as_arm(t, argp, start)) {
            basicasm_cpu = BASICASM_CPU_ARM;        /* an ARM program, as described above */
            arm_by_recognition = 1;
            arm_program = program_fingerprint(argp);
            return BASICASM_ARM;
        }
    }
    if (!spell(t, start, end))
        return BASICASM_ERSYNT;
    uint8_t out[32];
    size_t n = 0;
    asm_ctx c;
    memset(&c, 0, sizeof c);
    c.eval = dry_cb;
    c.addr = ros_ld32(argp + WS_P);
    int dry = encode(&c, t->buf, t->n, out, sizeof out, &n);
    if (dry != ASM_OK && !cpu_chosen && basicasm_cpu == BASICASM_DEFAULT_CPU &&
        reads_as_arm(t->buf, t->n, c.ekind == ASM_EK_MNEMONIC)) {
        basicasm_cpu = BASICASM_CPU_ARM;            /* an ARM program, as described above */
        arm_by_recognition = 1;
        arm_program = program_fingerprint(argp);
        return BASICASM_ARM;
    }
    if (dry != ASM_OK && c.ekind == ASM_EK_MNEMONIC)
        return BASICASM_ERASS1;
    int opt = ros_ld8(argp + WS_BYTESM);
    if (basicasm_cpu == BASICASM_CPU_A64) {
        /* Align to a word first, as the ARM assembler aligns an ARM
         * instruction before its operands (s.Assembler CASMGT1, ALIGN).
         * The padding zeros go at O% with offset assembly, and at P%
         * otherwise. P% is aligned in either case. */
        uint32_t p = ros_ld32(argp + WS_P);
        uint32_t at = opt & OPT_TO_O ? ros_ld32(argp + WS_O) : p;
        while (at & 3)
            ros_st8(at++, 0);
        if (opt & OPT_TO_O)
            ros_st32(argp + WS_O, at);
        ros_st32(argp + WS_P, (p + 3) & ~3u);
    }
    t->here = ros_ld32(argp + WS_P);
    if (missing_operand(t))
        return BASICASM_FACERR;

    memset(&c, 0, sizeof c);
    c.eval = eval_cb;
    c.ud = t;
    c.addr = t->here;
    c.addr32 = 1;
    t->c = &c;
    int rc = encode(&c, t->buf, t->n, out, sizeof out, &n);
    int err = t->err;
    uint32_t here = t->here;
    if (err)
        return err;
    if (rc != ASM_OK)
        return error_for(c.ekind);
    /* A value that did not fit is reported as the ARM assembler reports
     * its own. That is with OPT's errors bit, or always for a branch. */
    if (c.soft && ((c.soft & ASM_SOFT_ALWAYS) || (opt & OPT_ERRORS)))
        return error_for(c.soft & 0xFF);

    s->r[10] = ros_ld8(end);            /* for ASMCHK: ':', CR or a comment */
    s->r[11] = end + 1;
    if (basicasm_cpu == BASICASM_CPU_A64) {
        s->r[1] = (uint32_t)out[0] | (uint32_t)out[1] << 8 | (uint32_t)out[2] << 16 |
                  (uint32_t)out[3] << 24;
        return BASICASM_WORD;
    }
    /* x86-64: the bytes go in STRACC as a string for CASMX to place, and R2
     * is the end of the string */
    uint32_t acc = argp + WS_STRACC;
    for (size_t i = 0; i < n; i++)
        ros_st8(acc + (uint32_t)i, out[i]);
    s->r[2] = acc + (uint32_t)n;
    pending.line = s->r[12];
    pending.p = here;
    pending.n = n < sizeof pending.b ? n : sizeof pending.b;
    memcpy(pending.b, out, pending.n);
    return BASICASM_BYTES;
}

/* CASMX's listing. The ARM assembler writes an instruction's word here.
 * This writes an x86-64 instruction's bytes instead, in the order they lie
 * in memory. They take the same eight columns when they fit, which is four
 * bytes or fewer. CASMX1 then writes the label and the source. The function
 * returns 0 for anything else, and CASMX lists that in the usual way. */
int basicasm_list_bytes(struct ros_cpu *s)
{
    if (!pending.n || s->r[12] != pending.line || ros_ld32(s->r[8] + WS_P) != pending.p)
        return 0;
    char hex[40];
    size_t k = 0;
    for (size_t i = 0; i < pending.n; i++)
        k += (size_t)snprintf(hex + k, sizeof hex - k, "%02X", pending.b[i]);
    while (k < 8)
        hex[k++] = ' ';
    for (size_t i = 0; i < k; i++)
        ros_swi(s, 0x100u + (unsigned char)hex[i]);         /* OS_WriteI */
    pending.n = 0;
    return 1;
}

/* ']' ends the block, for every CPU. The code runs where P% put it, from
 * where P% started. O% only holds it. This applies if it is this machine's
 * code, which is x86-64 in the box on x86-64 and AArch64 in a box on
 * AArch64. CALL and USR enter it. ARM code (*BasicAsmCPU ARM) runs too, in
 * the ARM container. The range is ARM code from now on. CALL and USR run it
 * there, and so does an ARM module that is given its address. Anything else
 * written there is another CPU's code and can no longer run. Any range
 * registered over it is forgotten. CALL then finds no code there. The bytes
 * are not run as the wrong machine's code. */
void basicasm_ket(struct ros_cpu *s, uint32_t start, uint32_t end)
{
    (void)s;
    pending.n = 0;
    block_end();
    if (end <= start)
        return;
    if (basicasm_cpu == BASICASM_NATIVE_CPU) {
        ros_armrun_code_forget(start, end);
        os_error *e = ros_capp_code_assembled(start, end);
        if (e)
            ros_raise(e);
    } else if (basicasm_cpu == BASICASM_CPU_ARM) {
        ros_capp_code_forget_assembled(start, end);
        ros_armrun_code_forget(start, end);
        if (!ros_armrun_code_add(start, end))
            ros_raise(ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for more ARM code"));
    } else {
        ros_capp_code_forget_assembled(start, end);
        ros_armrun_code_forget(start, end);
    }
}
