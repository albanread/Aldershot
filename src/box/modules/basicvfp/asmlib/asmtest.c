/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asmtest.c: runs the assemblers over a corpus of LLVM MC goldens.
 *
 *   asmtest aarch64.tsv [filter]
 *   asmtest x86_64.tsv [filter]
 *
 * Each line is "instruction text<TAB>hex bytes", followed by optional
 * fields. A field of the form "@hex" is the address at which the
 * instruction is assembled. It is 0 if there is no such field. It is used
 * for the PC-relative forms, whose operand is an address. A name L_<hex> in
 * a field is a label at that address, as a BASIC variable is. In x86-64,
 * [rip + L_10400] means the label's address, and [rip + 16] is a
 * displacement. The file's name says which assembler to use: a name that
 * contains "x86" means x86-64.
 *
 * A value error that the encoder assembled past (asm_ctx.soft) counts as a
 * failure. The exception is a line whose bytes are "-". That is an
 * instruction the encoder must refuse, either with an error or with a value
 * error (see goldens/refuse_*.tsv).
 *
 * It reports every mismatch and then a summary. The exit status is the
 * number of failures, capped at 100.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "asma64.h"

int asmx64_insn(asm_ctx *c, const char *text, size_t len,
                uint8_t *out, size_t cap, size_t *outlen);

/* Evaluates literals, and also L_<hex>, which is a named label at that address (see asm.h). */
static int eval(void *ud, const char *b, const char *e, int64_t *out, char *err, size_t errcap)
{
    while (b < e && (*b == ' ' || *b == '#' || *b == '+'))
        b++;
    while (e > b && e[-1] == ' ')
        e--;
    if (e - b > 2 && b[0] == 'L' && b[1] == '_') {
        int64_t v = 0;
        for (const char *p = b + 2; p < e; p++) {
            int h = asm__hexval(*p);
            if (h < 0)
                return asm_eval_literal(ud, b, e, out, err, errcap);
            v = v << 4 | h;
        }
        *out = v;
        return ASM_EVAL_NAMED;
    }
    return asm_eval_literal(ud, b, e, out, err, errcap);
}

static int hexnib(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s corpus.tsv [filter]\n", argv[0]);
        return 2;
    }
    int isx64 = strstr(argv[1], "x86") != NULL;
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    char line[512];
    int total = 0, pass = 0, softfail = 0;
    int shown = 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        char *hex = tab + 1;
        while (*hex == ' ')
            hex++;
        uint8_t want[32];
        int nwant = 0;
        int refuse = hex[0] == '-' && (hex[1] == '\n' || hex[1] == '\t' || !hex[1]);
        if (refuse)
            want[nwant++] = 0, hex++;
        for (; !refuse && *hex && *hex != '\t' && *hex != '\n' && nwant < 32; hex += 2) {
            int h = hexnib(hex[0]), l = hexnib(hex[1]);
            if (h < 0 || l < 0)
                break;
            want[nwant++] = (uint8_t)(h << 4 | l);
        }
        if (!nwant)
            continue;
        uint64_t at = 0;
        for (char *f = strchr(hex, '\t'); f; f = strchr(f + 1, '\t'))
            if (f[1] == '@')
                at = strtoull(f + 2, NULL, 16);
        if (argc >= 3 && !strstr(line, argv[2]))
            continue;
        total++;
        asm_ctx c;
        memset(&c, 0, sizeof c);
        c.eval = eval;
        c.addr = at;
        uint8_t got[32];
        size_t gotlen = 0;
        int rc = isx64 ? asmx64_insn(&c, line, strlen(line), got, sizeof got, &gotlen)
                       : asma64_insn(&c, line, strlen(line), got, sizeof got, &gotlen);
        if (refuse ? rc != ASM_OK || c.soft
                   : rc == ASM_OK && !c.soft && gotlen == (size_t)nwant &&
                         memcmp(got, want, nwant) == 0) {
            pass++;
            continue;
        }
        softfail++;
        if (shown < 40) {
            shown++;
            if (refuse) {
                printf("FAIL(ok)  %-46s want it refused, got", line);
                for (size_t i = 0; i < gotlen; i++)
                    printf(" %02x", got[i]);
                printf("\n");
            } else if (rc != ASM_OK)
                printf("FAIL(err) %-46s want %02x.. got error: %s\n",
                       line, want[0], c.err);
            else if (c.soft)
                printf("FAIL(val) %-46s want %02x.. got value error: %s\n",
                       line, want[0], c.softerr);
            else {
                printf("FAIL      %-46s want", line);
                for (int i = 0; i < nwant; i++)
                    printf(" %02x", want[i]);
                printf("  got");
                for (size_t i = 0; i < gotlen; i++)
                    printf(" %02x", got[i]);
                printf("\n");
            }
        }
    }
    fclose(f);
    printf("%s: %d/%d pass (%d fail)\n", argv[1], pass, total, total - pass);
    return total - pass > 100 ? 100 : total - pass;
}
