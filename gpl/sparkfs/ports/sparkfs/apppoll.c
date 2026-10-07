/*
 * Copyright 1996 Acorn Computers Ltd
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
 */
/* apppoll.c -- SparkFSApp's s/poll (Apps/SparkFSApp; RISC_OSLib's, as Acorn
 * released it in 1992), in C: wimp_poll and wimp_pollidle, and the two
 * switches for the floating point state around them.  SparkFSApp links no
 * RISC_OSLib, so it carries these itself (appswi.c says why).
 *
 * wimp_poll: R0 the mask, R1 the event block's data (the block + 4),
 * XWimp_Poll; then R0 stored at R1 - 4, the block's reason word, whatever
 * the outcome (SUB a2, a2, #4; STR a1, [a2]), and MOVVC a1, #0: the error,
 * else 0.  wimp_pollidle: the same with R2 the earliest time and
 * XWimp_PollIdle -- except that its CMP lk, #0 (whether to restore the
 * floating point state) comes between the SWI and the MOVVC and clears V,
 * so it returns 0 either way; an error is only in the reason word.
 *
 * Around the SWI the ObjAsm saves the FPA's f4-f7 and status when
 * _kernel_fpavailable says there is floating point and poll_preserve_fp is
 * set (the default), because on RISC OS another task may use those
 * registers while this one is paged out.  In the box a task is a thread of
 * its own: the other tasks never touch its floating point registers, no
 * value is kept in one across a call (SysV, AAPCS64 caller-saved or, for
 * AArch64's d8-d15, callee-saved by the gate's call like any other), and
 * the gate keeps the program's floating point control state across the
 * SWI.  So the state is preserved whichever way the switch is set; the
 * switch is kept, as rosgd/rlib/poll.c keeps rlib's. */
#include <stddef.h>
#include "kernel.h"
#include "swis.h"
#include "os.h"
#include "wimp.h"
#ifdef __aarch64__
#include "rlib_a64x32.h"            /* A64X32: rosgd/abi/a64x32 */
#else
#include "rlib_x32.h"
#endif

_Static_assert(offsetof(wimp_eventstr, data) == 4, "s/poll puts the data at the block + 4");
_Static_assert(sizeof(wimp_etype) == 4, "the reason word is a word");

static int poll_preserve_fp = 1;       /* poll_preserve_fp DCD 1 */

/* R1 back from the Wimp, less 4: the reason word, V or not */
static void reason(u32 r[10])
{
    *(u32 *)(unsigned long)(r[1] - 4) = r[0];
}

os_error *wimp_poll(wimp_emask mask, wimp_eventstr *result)
{
    u32 r[10];
    u32 psr;
    rlib_regs(r, (u32)mask, ADDR(result) + 4, 0, 0, 0, 0);
    psr = rlib_swi(Wimp_Poll | X_BIT, r);
    reason(r);
    return (psr & PSR_V) ? (os_error *)(unsigned long)r[0] : 0;   /* MOVVC a1, #0 */
}

os_error *wimp_pollidle(wimp_emask mask, wimp_eventstr *result, int earliest)
{
    u32 r[10];
    rlib_regs(r, (u32)mask, ADDR(result) + 4, (u32)earliest, 0, 0, 0);
    (void)rlib_swi(Wimp_PollIdle | X_BIT, r);
    reason(r);
    return 0;                          /* CMP lk, #0 cleared V before MOVVC */
}

void wimp_save_fp_state_on_poll(void)
{
    poll_preserve_fp = 1;
}

void wimp_corrupt_fp_state_on_poll(void)
{
    poll_preserve_fp = 0;
}
