/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asma64_int.h: internals shared between asma64.c and asma64_mv.c. */
#ifndef BASICASM_ASMA64_INT_H
#define BASICASM_ASMA64_INT_H

#include "asm.h"

enum {
    RK_NONE = 0, RK_X, RK_W,
    RK_B, RK_H, RK_S, RK_D, RK_Q, RK_V,
};

enum shiftkind_ { SHK_NONE = 0, SHK_LSL, SHK_LSR, SHK_ASR, SHK_ROR };
enum extkind_ {
    EXK_NONE = -1, EXK_UXTB = 0, EXK_UXTH = 1, EXK_UXTW = 2, EXK_UXTX = 3,
    EXK_SXTB = 4, EXK_SXTH = 5, EXK_SXTW = 6, EXK_SXTX = 7,
};

typedef struct a64opnd {
    int t;              /* O_REG / O_IMM / O_MEM / O_SFX */
    int regn, kind, vbits, vlanes, vidx;
    int64_t imm;
    int imm_known;
    int listn[4], listcnt;
    int is_shift;
    int shx;
    int64_t amt; int amt_known;
    int basereg, base_x;
    int idxreg, idxkind, idx_valid;
    int idxsh_is_shift, idxsh, idxsh_amt_known, idxsh_amt;
    int64_t disp; int disp_known;
    int wb_pre, wb_post;
    const char *b, *e;
} a64opnd;

enum { A64_O_REG, A64_O_IMM, A64_O_MEM, A64_O_SFX };

int asma64_dispatch(asm_ctx *c, const char *mn, const char *mne,
                    a64opnd *o, int n, uint8_t *out, size_t cap, size_t *outlen);
int asma64_prfop(const a64opnd *o);
int asma64_mem_fp_vec(asm_ctx *c, const char *name, const char *base,
                      const char *sfx, a64opnd *o, int n,
                      uint8_t *out, size_t cap, size_t *outlen);

#endif
