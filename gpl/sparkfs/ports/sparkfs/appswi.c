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
/* appswi.c -- SparkFSApp's s/swi (Apps/SparkFSApp), in C: the SWI veneers
 * the SparkFS application's C calls.  SparkFSApp links no RISC_OSLib (its
 * Makefile gives no LIBS: CApp's APP_LIBS is the C library's stubs alone);
 * it compiles against RISC_OSLib's headers (os.h, bbc.h) and brings these
 * ten of rlib's veneers itself, Acorn's "cwimp" s.swi cut down:
 *
 *   bbc_get     XOS_ReadC; on V the error block's address, else the
 *               character, with &100 added when C is set (an Escape)
 *   bbc_vdu     XOS_WriteC of R0 (the whole int: OS_WriteC writes its low
 *               byte); the error, else 0
 *   bbc_vduw    XOS_WriteC of R0, then of what the SWI left in R0 shifted
 *               down 8 (OS_WriteC preserves R0), the second entered with the
 *               flags the first left; an error from the first at once
 *   os_swi1, os_swix1 ... os_swix4
 *               one entry: the X forms ORR the X bit in and fall through.
 *               R12 the SWI number, R0-R5 from a2, a3, a4 and the first
 *               three stack words, then XOS_CallASWIR12; MOVVC a1, #0
 *   os_swi3r, os_swix3r
 *               R0-R2 from a2-a4, the SWI; on V its error and nothing stored
 *               (Return ... VS); else R0-R2 to the three pointers on the
 *               stack, each that is not 0 (TEQ; STRNE), and 0
 *
 * os_swi2-4 are labels in the ObjAsm but not exported, so not here.
 *
 * In the box an application reaches the OS through the gate (rosgd's
 * include/rosgd/capp.h; abi/x32/rlib_x32.h, which rlib's own veneers in C,
 * rosgd/rlib/swi.c, use too): R0-R9 in and out, the PSR after the SWI back.
 * XOS_CallASWIR12 is the kernel calling the SWI in R12 as itself, its top
 * byte cleared, so here it is that SWI through the gate.  What the ARM
 * veneer leaves in registers its arguments do not fill (os_swix1's R1-R5:
 * the caller's a3, a4 and stack) is what no SWI it is documented for reads;
 * those are 0 here.  The prototypes are RISC_OSLib's (variadic), so the
 * arguments are read with va_arg as the ARM code read them from a2-a4 and
 * the stack.  A form without X makes the SWI without X: an error goes to the
 * error handler and does not come back, as it does on RISC OS. */
#include <stdarg.h>
#include "kernel.h"
#include "swis.h"
#include "os.h"
#include "bbc.h"
#ifdef __aarch64__
#include "rlib_a64x32.h"            /* A64X32: rosgd/abi/a64x32 */
#else
#include "rlib_x32.h"
#endif

#define SWI_NUMBER(n) ((u32)(n) & 0x00FFFFFFu)   /* CallASWIR12: the top byte cleared */

static os_error *error_of(u32 psr, u32 r0)
{
    return (psr & PSR_V) ? (os_error *)(unsigned long)r0 : 0;   /* MOVVC a1, #0 */
}

/* ---- bbc_get, bbc_vdu, bbc_vduw ------------------------------------------- */

int bbc_get(void)
{
    u32 r[10];
    u32 psr;
    rlib_regs(r, 0, 0, 0, 0, 0, 0);
    psr = rlib_swi(OS_ReadC | X_BIT, r);
    if (!(psr & PSR_V) && (psr & PSR_C))          /* Return VS; ORRCS a1, a1, #&100 */
        r[0] |= 0x100;
    return (int)r[0];
}

os_error *bbc_vdu(int c)
{
    u32 r[10];
    rlib_regs(r, (u32)c, 0, 0, 0, 0, 0);
    return error_of(rlib_swi(OS_WriteC | X_BIT, r), r[0]);
}

os_error *bbc_vduw(int c)
{
    u32 r[10];
    u32 psr;
    rlib_regs(r, (u32)c, 0, 0, 0, 0, 0);
    psr = rlib_swi(OS_WriteC | X_BIT, r);
    if (psr & PSR_V)                              /* Return ,LinkNotStacked,VS */
        return (os_error *)(unsigned long)r[0];
    r[0] >>= 8;                                   /* MOV a1, a1, LSR #8 */
    return error_of(rlib_swi_psr(OS_WriteC | X_BIT, r, psr), r[0]);
}

/* ---- os_swi1, os_swix1-4: registers in, the error out --------------------- */

static os_error *swi_in(u32 n, int nin, va_list ap)
{
    u32 a[6], r[10];
    int i;
    for (i = 0; i < 6; i++)
        a[i] = i < nin ? (u32)va_arg(ap, int) : 0;
    rlib_regs(r, a[0], a[1], a[2], a[3], a[4], a[5]);
    return error_of(rlib_swi(SWI_NUMBER(n), r), r[0]);
}

#define SWI_IN(name, xbit, nin)                                  \
    os_error *(name)(int swicode, ...)                           \
    {                                                            \
        va_list ap;                                              \
        os_error *e;                                             \
        va_start(ap, swicode);                                   \
        e = swi_in((u32)swicode | (xbit), nin, ap);              \
        va_end(ap);                                              \
        return e;                                                \
    }
SWI_IN(os_swi1, 0, 1)
SWI_IN(os_swix1, X_BIT, 1)
SWI_IN(os_swix2, X_BIT, 2)
SWI_IN(os_swix3, X_BIT, 3)
SWI_IN(os_swix4, X_BIT, 4)

/* ---- os_swi3r, os_swix3r: three registers in, three pointers out ---------- */

static os_error *swi_3r(u32 n, va_list ap)
{
    u32 a[3], r[10];
    int *out[3];
    int i;
    for (i = 0; i < 3; i++)
        a[i] = (u32)va_arg(ap, int);
    for (i = 0; i < 3; i++)                       /* LDMIA ip, {v1, v2, v3} */
        out[i] = va_arg(ap, int *);
    rlib_regs(r, a[0], a[1], a[2], 0, 0, 0);
    if (rlib_swi(SWI_NUMBER(n), r) & PSR_V)       /* Return "v1-v6",,VS */
        return (os_error *)(unsigned long)r[0];
    for (i = 0; i < 3; i++)                       /* TEQ vN, #0; STRNE aN, [vN] */
        if (out[i])
            *out[i] = (int)r[i];
    return 0;
}

os_error *(os_swi3r)(int swicode, ...)
{
    va_list ap;
    os_error *e;
    va_start(ap, swicode);
    e = swi_3r((u32)swicode, ap);
    va_end(ap);
    return e;
}

os_error *(os_swix3r)(int swicode, ...)
{
    va_list ap;
    os_error *e;
    va_start(ap, swicode);
    e = swi_3r((u32)swicode | X_BIT, ap);
    va_end(ap);
    return e;
}
