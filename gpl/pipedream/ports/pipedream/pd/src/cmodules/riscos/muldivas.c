/* muldivas.c */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Copyright (C) 1991-1998 Colton Software Limited
 * Copyright (C) 1998-2015 R W Colton
 * Copyright (C) 2026 the PipeDream box port */

/* C port of cmodules/riscos/s/muldivas.s for the box (x32): the
 * 32-bit * 32-bit / 32-bit helpers a la BCPL the ARM code did with
 * UMULL and a hand-rolled 64-by-32 division.  The machine does these
 * natively now.  Overflow is reported with its sign, as the assembler
 * did: muldiv64_limiting() (cmodules/muldiv.c) turns that into the
 * +/-S32_MAX clamp. */

#include "common/gflags.h"

#include "cmodules/muldiv.h"

static S32 muldiv64__overflow = 0;

extern void
muldiv64_init(void)
{
}

_Check_return_
extern S32
muldiv64(
    _InVal_ S32 dividend,
    _InVal_ S32 numerator,
    _InVal_ S32 denominator)
{
    S64 product = (S64) dividend * (S64) numerator;
    S64 quotient;

    if(0 == denominator)
    {   /* the ARM code's divide-by-zero path: no result, overflow signed
         * by the product, so limiting clamps to an end */
        muldiv64__overflow = (product < 0) ? -1 : (product > 0) ? +1 : 0;
        return(0);
    }

    quotient = product / denominator;   /* BCPL muldiv: toward zero */

    if((quotient > (S64) S32_MAX) || (quotient < (S64) -S32_MAX))
        muldiv64__overflow = (quotient < 0) ? -1 : +1;
    else
        muldiv64__overflow = 0;

    return((S32) quotient);
}

_Check_return_
extern S32
muldiv64_overflow(void)
{
    return(muldiv64__overflow);
}

/* end of muldivas.c */
