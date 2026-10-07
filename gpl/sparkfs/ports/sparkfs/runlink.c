/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "Licence").
 * You may not use this file except in compliance with the Licence.
 *
 * You can obtain a copy of the licence at
 * RiscOS/Sources/FileSys/ImageFS/SparkFS/LICENCE.
 * See the Licence for the specific language governing permissions
 * and limitations under the Licence.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the Licence file. If applicable, add the
 * following below this CDDL HEADER, with the fields enclosed by
 * brackets "[]" replaced with your own identifying information:
 * Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 *
 * Copyright 1992 David Pilling.  All rights reserved.
 * Use is subject to license terms.
 */
/* runlink.c -- SparkFS's s/runlink (FileSys/ImageFS/SparkFS/SparkFS), in C.
 *
 * The ObjAsm:
 *
 *   ; _kernel_oserror * runlink(linkblock * linkb,linkfn fn,int pw)
 *   runlink MOV ip,sp ; STMFD sp!,{v1-v6,fp,ip,lr,pc} ; SUB fp,ip,#4
 *           MOV R12,a3                  ; the codec's workspace
 *           MOV lr,pc ; MOV pc,a2       ; call fn, a1 = linkb
 *           LDMEA fp,{v1-v6,fp,sp,pc}   ; a1 its error, or 0
 *
 * calls one of a codec module's link functions (SparkZip's veneer_loadfile,
 * say: SparkLib's s/sinterface, sinterface.c here) with the block in a1
 * and R12 the codec's private word, keeping SparkFS's own registers.  The
 * veneer on the other side uses R12 only to switch the C library's
 * relocation offsets (the static base) to the codec's own.
 *
 * In the box both modules are C of one ABI (x32, A64X32), and a call from
 * one to the other is a C call: the result is the function's.  A module's
 * own statics are its own wherever it is called from (roscc links them
 * PC-relative: no relocation offsets to switch), so what the veneer did
 * for them is done; the C library's per-client statics are those of the
 * module the box entered -- SparkFS's -- for the call, which the veneer
 * cannot change (the runtime alone sets the static base, on its entries:
 * include/rosgd/capp.h).  The codecs' library state lives within each call
 * (memory a call allocates it frees: inflate's tables, deflate's trees),
 * so they keep to SparkFS's for the whole of every link call.  pw, which
 * the ObjAsm put in R12, is not needed for that.
 *
 * CDDL 1.0, as SparkFS's own (RiscOS/Sources/FileSys/ImageFS/SparkFS/
 * LICENCE); the original: Copyright 1992 David Pilling.  All rights
 * reserved.  Use is subject to license terms.
 */
#include "kernel.h"

#include "Interface/SparkFS.h"

_kernel_oserror *runlink(linkblock *linkb, linkfn fn, int pw);

_kernel_oserror *runlink(linkblock *linkb, linkfn fn, int pw)
{
    (void)pw;
    return fn(linkb);
}
