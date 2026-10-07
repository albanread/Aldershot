/* cs-riscasm.c */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Copyright (C) 1988-1998 Colton Software Limited
 * Copyright (C) 1998-2015 R W Colton
 * Copyright (C) 2026 the PipeDream box port */

/* C port of WimpLib/s/cs-riscasm.s for the box (x32): the escape and
 * event environment handlers, and the SWI veneers the non-Norcroft build
 * links to.  The escape handler is entered by the box as a nested entry
 * (rosgd/include/rosgd/capp.h, package R2a):
 *
 *     void entry(uint32_t regs[17]);   R0-R15, then the PSR
 *
 * with R11 the escape state, as RISC OS gives it (the box passes &FF or
 * 0; the ARM code masked with &40, which both satisfy). */

#include "include.h"

#include "swis.h"

#include "kernel.h"

#include "cmodules/monotime.h"

/* ------------------------------------------------------------------------ */
/* Escape: EscH(&ctrlflag) installs a handler that mirrors the escape state */

static int * esc_ctrlflagp = 0;

static void esc_handler(uint32_t regs[17])
{
    if(esc_ctrlflagp)
        *esc_ctrlflagp = (int) (regs[11] & 0x40u);

    /* nested entries return through the box's gate; falling off the end
     * does that (capp.h: "what the code leaves in R0-R15 and the PSR's
     * N Z C V comes back") */
}

extern void
EscH(
    int *addressofflag)
{
    _kernel_oserror * e;
    _kernel_swi_regs r;

    esc_ctrlflagp = addressofflag;

    r.r[0] = 0;
    r.r[1] = 0;
    r.r[2] = (int) esc_handler;
    r.r[3] = 0;

    e = _kernel_swi(OS_ChangeEnvironment, &r, &r);
    (void) e;
}

/* ------------------------------------------------------------------------ */
/* Event: EventH() installs a do-nothing handler, replacing the C runtime's */

static void event_handler(uint32_t regs[17])
{
    (void) regs;
}

extern void *
EventH(void)
{
    _kernel_oserror * e;
    _kernel_swi_regs r;

    r.r[0] = 0;
    r.r[1] = 0;
    r.r[2] = (int) event_handler;
    r.r[3] = 0;

    e = _kernel_swi(OS_ChangeEnvironment, &r, &r);
    if(e)
        return(0);

    return((void *) r.r[2] ? (void *) event_handler : 0);
}

/* ------------------------------------------------------------------------ */
/* SWI veneers (the non-NORCROFT_INLINE_SWIX ones from cs-riscasm.s) */

extern MONOTIME
monotime(void)
{
    int now = 0;

    (void) _swix(OS_ReadMonotonicTime, _OUT(0), &now);

    return((MONOTIME) now);
}

extern _kernel_oserror *
os_writeN(
    _In_reads_(count) const char * s,
    _InVal_     U32 count)
{
    return(_swix(OS_WriteN, _INR(0,1), s, count));
}

extern _kernel_oserror *
os_plot(
    _InVal_     int code,
    _InVal_     int x,
    _InVal_     int y)
{
    return(_swix(OS_Plot, _INR(0,2), code, x, y));
}

extern void
riscos_hourglass_off(void)
{
    (void) _swix(Hourglass_Off, 0);
}

extern void
riscos_hourglass_on(void)
{
    (void) _swix(Hourglass_On, 0);
}

extern void
riscos_hourglass_percentage(int percentage)
{
    (void) _swix(Hourglass_Percentage, _IN(0), percentage);
}

extern void
riscos_hourglass_smash(void)
{
    (void) _swix(Hourglass_Smash, 0);
}

extern void
riscos_hourglass_start(int after_cs)
{
    (void) _swix(Hourglass_Start, _IN(0), after_cs);
}

extern _kernel_oserror *
font_LoseFont(
    font f)
{
    return(_swix(Font_LoseFont, _IN(0), f));
}

extern _kernel_oserror *
font_SetFont(
    font f)
{
    return(_swix(Font_SetFont, _IN(0), f));
}

/* end of cs-riscasm.c */
