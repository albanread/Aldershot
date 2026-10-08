/* Copyright 2001 Pace Micro Technology plc
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
 * This file is a translation into C of RISC OS Open's BBC BASIC source
 * (Sources/Programmer/BASIC: s.Expr).
 */

/* expr_lv.c: BASIC's variable evaluation, translated by hand from
 * Expr.s:283-470 (RISC OS 5.31's BASIC, the VFP build).  It holds
 * AELV, LVBLNK, LVCONT, the LVNOTCACHE machine, and the ! and ? suffix
 * handlers.
 *
 * This code lies behind every variable reference.  It decides whether
 * the item at AELINE is an l-value and finds it.  It returns with IACC
 * pointing at the variable's data and TYPE saying what it is:
 *   - 0 a byte
 *   - 4 an integer
 *   - 5 or 8 a float
 *   - 128 a string
 *   - 129 a $-string
 *   - 256 and above an array to be indexed.
 * An EQ status means "not an l-value".  If C is then clear, the name
 * is not in the list and may be created.  If C is set, the item is
 * silly.
 *
 * The name search (LOOKU1/LOOKU2) keeps the original's unrolled trick.
 * The entry's name words are rotated a byte at a time into the top of
 * R5 and compared as they come.  So the loop fetches a fresh word only
 * every four characters.  The LOOKA0..3 exits adjust R0 by the number
 * of characters the match ran past the last word.  The list itself
 * makes the compare case-folded, because entries store their names the
 * way CRUNCH wrote them.
 *
 * The quirks kept, with their lines:
 *   - A name whose first character is below "@" is not an l-value.
 *     But the unary !, $, ? and | are tried, and @% is the built-in
 *     integer block (LVFD, LVFDAT, Expr.s:428-436).
 *   - A single-character name matches only single-character entries
 *     (TST R5,#&FF, LOOKU5), and is always a float's slot.
 *   - Statics such as %A address INTVAR by letter (LVSTATICINT).
 *   - The found pointer is word-aligned for integer and float types
 *     only (SUBNE/BICEQ at the tail of LOOKA3).
 *   - The ! and ? suffixes re-enter FACTOR for the index (BIPLIN,
 *     BIQUER).  They return TYPE 4 or 129, and the NE status of the
 *     compare of TYPE with 6.
 *   - The cache stores the found entry (IACC, offset, key, TYPE) for
 *     the fast path in LVCONT.  It is keyed by the name's address.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "expr_lv.h"

void basicvfp_FACTOR(struct ros_cpu *s);
void basicvfp_ARLOOKCACHE(struct ros_cpu *s);
void basicvfp_ERARRY(struct ros_cpu *s);
void basicvfp_VARIND(struct ros_cpu *s);
void basicvfp_hand_INTEGZ(struct ros_cpu *s);
void basicvfp_hand_INTEGY(struct ros_cpu *s);

/* The BL FACTOR sites.  Before performance phase P1 these pushed
 * setjmp'd resume frames.  The reason was found in the
 * unit-expr-factor-rest work.  The index and operand evaluations for
 * !, $ and ? can be FN calls, and the GTARGS unwinder resumed them
 * non-locally through R14.  The lift's sites are UNQUER at &FC1022BC
 * and BIQUER at &FC102240.  A hand caller that left out the frame had
 * the resume CALL the marker, which showed as the `PRINT !FNa(0)`
 * differential.  The FN return now rides the C stack (FNRET/GTARGS).
 * See EFR_RESUME in expr_factor_rest.c for the full argument.  So the
 * frames are gone.  The helpers stay noinline, because the callers'
 * continuations must stay plain code. */
__attribute__((noinline)) static void lv_unquer_factor(struct ros_cpu *s)
{
    s->r[14] = 0xFC1022BCu;
    basicvfp_FACTOR(s);
    ros_check_return(s, 0xFC1022BCu);
}

__attribute__((noinline)) static void lv_biquer_factor(struct ros_cpu *s)
{
    s->r[14] = 0xFC102240u;
    basicvfp_FACTOR(s);
    ros_check_return(s, 0xFC102240u);
}

#define CACHEMASK 255u
#define TFP 0x80000000u
#define TFPLV 8u
#define LVTABLE 0xFC101F68u
#define PROCPTR_OFF 0x200u
#define INTVAR_OFF 0x100u

/* AELV lives in units/basic_lookup.c (unit 4c); it calls the lift's
 * LVBLNK, which continues into the LVCONT thunk below. */

/* LVBLNK: skip the spaces, then go on to LVCONT. */
void basicvfp_hand_LVBLNK(struct ros_cpu *s)
{
    uint32_t r10, r11 = s->r[11];
    do {
        r10 = ros_ld8(r11);
        r11 += 1;
    } while (r10 == 32);
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[15] = s->r[14];
    basicvfp_hand_LVCONT(s);
}

/* LVCONT: try the cache.  The entry is keyed by the name's address. */
void basicvfp_hand_LVCONT(struct ros_cpu *s)
{
    uint32_t r1 = s->r[8] + ((s->r[11] & CACHEMASK) << 4);
    uint32_t r0 = ros_ld32(r1);
    uint32_t r4 = ros_ld32(r1 + 8);
    uint32_t r9 = ros_ld32(r1 + 12);
    uint32_t roff = ros_ld32(r1 + 4);

    if (r4 != s->r[11]) {                   /* not this entry */
        s->r[0] = r0;
        s->r[1] = roff;
        s->r[4] = r4;
        s->r[9] = r9;
        s->r[15] = s->r[14];
        basicvfp_hand_LVNOTCACHE(s);
        return;
    }
    ros_adds(s, roff, 1);                   /* CMN R1,#1 */
    s->r[11] += roff;
    if (!s->n) {                            /* plain hit */
        s->r[0] = r0;
        s->r[1] = roff;
        s->r[4] = r4;
        s->r[9] = r9;
        s->r[15] = s->r[14];
        return;
    }
    s->r[11] -= TFP;                        /* an array's slot */
    s->r[0] = r0;
    s->r[1] = roff;
    s->r[4] = r4;
    s->r[9] = r9;
    s->r[15] = s->r[14];
    basicvfp_ARLOOKCACHE(s);
}

/* ---- the machine ------------------------------------------------------ */

void basicvfp_hand_LVNOTCACHE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2 = s->r[2], r3, r4 = s->r[11],
             r5, r6, r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r8 = s->r[8], r13 = s->r[13], r14 = s->r[14];

    r4 = r11;
    r1 = ros_subs(s, r10, 64);              /* rebase for VARPTR */
    if (r10 <= 64)
        goto LVFD;
    r3 = LVTABLE;
    r9 = ros_ld8(LVTABLE + r10);            /* the first char's type */
    if (r9 != 0)
        goto LVFLOATIND;
MULTI: /* find the end of the name */
    for (;;) {
        r5 = ros_ld8(r11);
        r11 += 1;
        r9 = ros_ld8(r3 + r5);
        if (r9 != 0)
            break;
    }
    if (r9 != TFPLV) {                      /* one more char ($ % ! etc) */
        r5 = ros_ld8(r11);
        r11 += 1;
    }
    ros_subs(s, r5, 40);                    /* CMP R5,#"(" */
    if (r5 == 40)
        goto BKTVAR;                        /* an array reference */
    r11 -= 1;                               /* a normal variable */
    r1 = ros_ld32(r8 - PROCPTR_OFF + (r1 << 2));
    r3 = r4 ^ r11;                          /* single character? */
    if (r3 == 0)
        goto LOOKU5;
    r3 = ros_ld8(r4);                       /* the second char */
    r4 += 1;
    ros_subs(s, r10, 91);                   /* CMP R10,#"Z"+1 */
    if (r10 < 91)
        ros_subs(s, r3, 37);                /* CMPCC R3,#"%" */
    if (r10 < 91 ? r3 == 37 : r10 == 91)
        goto LVSTATICINT;                   /* a % static */

LOOKU1: /* walk the list */
    r0 = r1;
    if (r0 == 0)
        goto LVNTFN;
    r1 = ros_ld32(r0);                      /* LDMIA R0!,{R1,R5} */
    r5 = ros_ror(ros_ld32(r0 + 4), 8);
    r0 += 8;
    ros_logic(s, r3 ^ (r5 >> 24), (r5 >> 23) & 1);
    if (!s->z)
        goto LOOKU1;                        /* first char differs */
    if ((r4 ^ r11) == 0)
        goto LOOKA2;
    r6 = r4;
LOOKU2: /* the unrolled four-characters-a-word compare */
    r5 = ros_ror(r5, 8);
    r2 = ros_ld8(r6);
    r6 += 1;
    ros_logic(s, r2 ^ (r5 >> 24), (r5 >> 23) & 1);
    if (!s->z)
        goto LOOKU1;
    if ((r6 ^ r11) == 0)
        goto LOOKA1;
    r5 = ros_ror(r5, 8);
    r2 = ros_ld8(r6);
    r6 += 1;
    ros_logic(s, r2 ^ (r5 >> 24), (r5 >> 23) & 1);
    if (!s->z)
        goto LOOKU1;
    if ((r6 ^ r11) == 0)
        goto LOOKA0;
    r5 = ros_ror(r5, 8);
    r2 = ros_ld8(r6);
    r6 += 1;
    ros_logic(s, r2 ^ (r5 >> 24), (r5 >> 23) & 1);
    if (!s->z)
        goto LOOKU1;
    r5 = ros_ld32(r0);                      /* the next name word */
    r0 += 4;
    if ((r6 ^ r11) == 0)
        goto LOOKA3;
    r5 = ros_ror(r5, 8);
    r2 = ros_ld8(r6);
    r6 += 1;
    ros_logic(s, r2 ^ (r5 >> 24), (r5 >> 23) & 1);
    if (!s->z)
        goto LOOKU1;
    if ((r6 ^ r11) != 0)
        goto LOOKU2;
    r0 -= 2;
LOOKA0:
    r0 += 1;                                /* adjust by 0 */
LOOKA1:
    r0 += 1;                                /* adjust by -1 */
LOOKA2:
    r0 += 1;                                /* adjust by -2 */
LOOKA3:
    if (r5 & 0xFF)                          /* the entry continues */
        goto LOOKU1;
    /* found: align for integer and float types */
    if ((r9 ^ 4) != 0 && (r9 ^ 8) != 0)
        r0 -= 3;
    else
        r0 &= ~3u;
    r4 -= 1;
    goto CHKQUE;

LOOKU5: /* single-character names, which are floats only.  The walk
 * loops back here, following each entry's link and testing the end
 * again */
    for (;;) {
        r0 = r1;
        if (r0 == 0)
            goto LVNTFN;
        r1 = ros_ld32(r0);
        r5 = ros_ld32(r0 + 4);
        r0 += 8;
        if ((r5 & 0xFF) == 0)
            break;                          /* a single-char entry */
    }
    /* fall through */

CHKQUE: /* the suffixes, and the cache store */
    r10 = ros_ld8(r11);
    if (r10 == 0x21) {                      /* "!" */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[9] = r9;
        s->r[10] = r10; s->r[11] = r11;
        s->r[15] = s->r[14];
        basicvfp_hand_BIPLIN(s);
        return;
    }
    r1 = ros_logic(s, r10 ^ 0x3F, s->c);    /* EORS R1,R10,#"?" */
    if (r1 == 0) {
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[9] = r9;
        s->r[10] = r10; s->r[11] = r11;
        s->r[15] = s->r[14];
        basicvfp_hand_BIQUER(s);
        return;
    }
    { /* the cache store */
        uint32_t slot = r8 + ((r4 & CACHEMASK) << 4);
        uint32_t off = r11 - r4;
        ros_st32(slot, r0);
        ros_st32(slot + 4, off);
        ros_st32(slot + 8, r4);
        ros_st32(slot + 12, r9);
        r1 = slot;
        r2 = off;
    }
    if (!s->z) {                            /* found: the NEQ answer */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[9] = r9;
        s->r[10] = r10; s->r[11] = r11;
        s->r[15] = r14;
        return;
    }
    /* EQ with the cache stored: the item is silly */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[9] = r9;
    s->r[10] = r10; s->r[11] = r11;
    s->r[15] = r14;
    return;

LVSTATICINT: /* %X: the built-in integer block, by letter */
    r1 = r10 - 64;
    r0 = r8 - INTVAR_OFF + (r1 << 2);
    r4 -= 1;
    goto CHKQUE;

LVFLOATIND: /* "|": a float indirection */
    ros_subs(s, r10, 124);
    if (r10 != 124)
        goto LVFD;
    r1 = TFPLV;
    goto UNQUER;

LVNTFN: /* not in the list: EQ, and C says whether it is silly */
    r0 = ros_adds(s, r0, 0);
    ros_logic(s, 0, s->c);                  /* TEQ R0,R0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[9] = r9; s->r[11] = r11;
    s->r[15] = r14;
    return;

LVFDAT: /* @%: the first integer of the built-in block */
    r5 = ros_ld8(r11);
    r11 += 1;
    if (r5 != 37)
        goto LVFDC1;
    r0 = ros_ld8(r11);
    ros_subs(s, r0, 40);
    if (r0 == 40)
        goto LVFDC1;
    r9 = 4;
    r0 = r8 - INTVAR_OFF;
    goto CHKQUE;

LVFD: /* not a name: try the unary !, $ and ?, or give up */
    if (s->z)
        goto LVFDAT;
    ros_subs(s, r10, 33);                   /* "!" */
    if (r10 == 33)
        goto UNPLIN;
    r1 = 129;
    ros_subs(s, r10, 36);                   /* "$" */
    if (r10 == 36)
        goto UNQUER;
    r1 = ros_logic(s, r10 ^ 0x3F, s->c);    /* "?" */
    if (r1 == 0)
        goto UNQUER;
    ros_subs(s, r10, r10);                  /* EQ, CS: silly */
    s->r[1] = r1; s->r[3] = r3; s->r[4] = r4; s->r[9] = r9;
    s->r[15] = r14;
    return;

LVFDC1:
    r11 -= 1;
    ros_subs(s, r10, r10);                  /* EQ, CS */
    s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[9] = r9; s->r[11] = r11;
    s->r[15] = r14;
    return;

UNPLIN:
    r1 = 4;
UNQUER: /* the index is FACTOR made an integer.  TYPE 6 means "no" */
    r13 -= 8;
    ros_st32(r13, r1);
    ros_st32(r13 + 4, r14);
    s->r[13] = r13;
    lv_unquer_factor(s);                        /* the BL FACTOR */
    r13 = s->r[13];
    s->r[14] = 0xFC1022C0u;                     /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r9 = ros_ld32(r13);                     /* LDMFD SP!,{TYPE,R14} */
    r14 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_subs(s, r9, 6);                     /* CMP TYPE,#6 ; ne status */
    s->r[0] = r0;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = r14;
    return;

BKTVAR: /* an array reference: find the array's entry */
    r3 = ros_ld8(r4);
    r4 += 1;
    r1 = ros_ld32(r8 - PROCPTR_OFF + (r1 << 2));
ARLOOKP1:
    if (r1 == 0)
        goto ARNOTFOUND;
    {
        uint32_t next = r1;
        r1 = ros_ld32(next);
        r0 = next + 8;
        r5 = ros_ror(ros_ld32(next + 4), 8);
        if (r3 ^ (r5 >> 24))
            goto ARLOOKP1;
        r5 &= 0xFF;
        if (r5 == 0)
            goto ARLOOKPY;
        if ((r4 ^ r11) == 0)
            goto ARLOOKP1;
        {
            uint32_t r6 = ros_ld8(r4);
            if (r6 ^ r5)
                goto ARLOOKP1;
            r2 = r4;
            r0 -= 2;
ARLOOKP2:
            for (;;) {
                r2 += 1;
                r6 = ros_ld8(r2);
                r5 = ros_ld8(r0);
                r0 += 1;
                if (r6 != r5)
                    break;
            }
            if (r5 != 0)
                goto ARLOOKP1;
            if ((r2 ^ r11) != 0)
                goto ARLOOKP1;
            goto ARLOOKPZ;
        }
    }

ARNOTFOUND: /* "()" of an unknown name: an array to be created */
    r0 = ros_ld8(r11);
    ros_subs(s, r0, 41);                    /* ")" */
    if (r0 != 41) {
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[9] = r9;
        s->r[11] = r11;
        s->r[15] = s->r[14];
        basicvfp_ERARRY(s);
        return;
    }
    r9 |= 0x100;
    goto LVNTFN;

ARLOOKPY:
    if ((r4 ^ r11) != 0)
        goto ARLOOKP1;
ARLOOKPZ: /* found the array: cache it, tagging the offset with TFP */
    r0 = (r0 + 3) & ~3u;
    r4 -= 1;
    r1 = r8 + ((r4 & CACHEMASK) << 4);
    r2 = r11 - r4 + TFP;
    ros_st32(r1, r0);
    ros_st32(r1 + 4, r2);
    ros_st32(r1 + 8, r4);
    ros_st32(r1 + 12, r9);
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[9] = r9; s->r[11] = r11;
    s->r[15] = s->r[14];
    basicvfp_ARLOOKCACHE(s);
}

/* ---- the suffix handlers (Expr.s:417-430) ----------------------------- */

/* BIPLIN handles "!", a word.  BIQUER handles "?", a byte.  Both skip
 * the suffix character.  They evaluate the base (VARIND, made an
 * integer) and the index (FACTOR, made an integer).  They answer
 * base+index, with the suffix's type tag and the "not TYPE 6"
 * status. */

static void biquer_common(struct ros_cpu *s)
{
    uint32_t r13 = s->r[13], r14 = s->r[14];
    uint32_t r1 = s->r[1], r0, r9;

    r13 -= 8;                               /* STMFD SP!,{R1,R14} */
    ros_st32(r13, r1);
    ros_st32(r13 + 4, r14);
    s->r[11] += 1;                          /* ADD AELINE,AELINE,#1 */
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_VARIND(s);                     /* the base */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r13 -= 4;                               /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    lv_biquer_factor(s);                    /* the BL FACTOR (index) */
    s->r[14] = 0xFC102244u;                 /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r1 = ros_ld32(r13);                     /* LDR R1,[SP],#4 */
    r13 += 4;
    r0 += r1;                               /* ADD IACC,IACC,R1 */
    r9 = ros_ld32(r13);                     /* LDMFD SP!,{TYPE,R14} */
    r14 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_subs(s, r9, 6);                     /* CMP TYPE,#6 ; ne status */
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = r14;
}

void basicvfp_hand_BIPLIN(struct ros_cpu *s)
{
    s->r[1] = 4;
    s->r[15] = s->r[14];
    biquer_common(s);
}

void basicvfp_hand_BIQUER(struct ros_cpu *s)
{
    s->r[15] = s->r[14];
    biquer_common(s);
}
