/* cs-poll.c */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Copyright (C) 1989-1998 Colton Software Limited
 * Copyright (C) 1998-2015 R W Colton
 * Copyright (C) 2026 the PipeDream box port */

/* C port of WimpLib/s/cs-poll.s for the box (x32).  Semantics preserved:
 * the hourglass is off across the poll and restarted afterwards (longer
 * after a redraw event), the FP-save bit is set in the mask for the Window
 * Manager, and a caller not listening for null events is polled with
 * Wimp_PollIdle, nulls no more than every 2cs. */

#include "include.h"

#include "swis.h"

#include "kernel.h"

#define AUTO_POLLIDLE_TIME  2                 /* cs delay till next null event */
#define NORMAL_DELAY_PERIOD 100               /* more suitable for normal apps than 33cs default */
#define REDRAW_DELAY_PERIOD 200               /* default (cs) */

#define Wimp_Poll_NullMask  1                 /* (1 << Wimp_ENull) */
#define Wimp_ERedrawWindow  1

#define flag_fpsavebit      24                /* Save/restore FP registers */
#define flag_fpsave         (1u << flag_fpsavebit)

extern _kernel_oserror *
wimp_poll_coltsoft(
    _In_        wimp_emask mask,
    _Out_       /*WimpPollBlock*/ void * block,
    _Inout_opt_ int * pollword,
    _Out_       int * event_code)
{
    _kernel_oserror * e;
    _kernel_swi_regs r;

    (void) _swix(Hourglass_Off, 0);

    if(0 == (mask & Wimp_Poll_NullMask))
    {   /* caller wants no null events: poll idle, nulls every so often */
        int now = 0;

        (void) _swix(OS_ReadMonotonicTime, _OUT(0), &now);

        r.r[0] = mask | flag_fpsave;
        r.r[1] = (int) block;
        r.r[2] = now + AUTO_POLLIDLE_TIME;
        r.r[3] = (int) pollword;

        e = _kernel_swi(Wimp_PollIdle, &r, &r);
    }
    else
    {
        r.r[0] = mask | flag_fpsave;
        r.r[1] = (int) block;
        r.r[2] = (int) pollword;

        e = _kernel_swi(Wimp_Poll, &r, &r);
    }

    *event_code = r.r[0];

    (void) _swix(Hourglass_Start, _IN(0),
                 (r.r[0] == Wimp_ERedrawWindow) ? REDRAW_DELAY_PERIOD : NORMAL_DELAY_PERIOD);

    return(e);
}

/* end of cs-poll.c */
