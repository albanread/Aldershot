/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "Licence").
 * You may not use this file except in compliance with the Licence.
 *
 * You can obtain a copy of the licence at
 * RiscOS/Sources/Lib/SparkLib/LICENCE.
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
/* sinterface.c -- SparkLib's s/sinterface (Lib/SparkLib), in C.
 *
 * A codec module (SparkZip, SparkTar ...) gives SparkFS a table of link
 * functions (SparkFS_Link, ADDLINK: an flink of veneer_ addresses), which
 * SparkFS calls through runlink with the block in a1 and R12 the codec's
 * private word.  Each ObjAsm veneer pushes r8 with its function's slot,
 * then at fsentry_common:
 *
 *   Push "r1-r7, sl, fp, ip, lr"
 *   sl = the stack's 1 MB base; save the two relocation words there
 *   r12 = [r12] ; LDMIB r12, {fp, r12} ; STMIA sl, {fp, r12}  -- the codec's
 *   fp = 0                               ; C backtraces stop here
 *   ADD r10, r10, #|_Lib$Reloc$Off$DP|   ; sl past the library's chunk
 *   BL  fsentry_branchtable[r8]          ; the C function, a1 = the block
 *   SUB r10, r10, #|_Lib$Reloc$Off$DP|
 *   STMIA sl, {v1, v2}                   ; SparkFS's relocation words back
 *   Pull "r1-r7, sl, fp, ip, lr" ; Pull "r8" ; MOV pc, lr
 *
 * that is, the C function with the codec's static data in force, a1 back.
 * In the box a codec's own statics need no switching (roscc links them
 * PC-relative), and its C library statics are SparkFS's for the call
 * (runlink.c says why that holds), so each veneer is the call itself.
 * The branch table's order is the flink's (Interface/SparkFS.h).
 *
 * CDDL 1.0, as SparkLib's own; the original: Copyright 1992 David Pilling.
 * All rights reserved.  Use is subject to license terms.
 */
#include "kernel.h"

#include "Interface/SparkFS.h"
#include "SparkLib/sinterface.h"

extern _kernel_oserror *updatecat(linkblock *);
extern _kernel_oserror *loadarchive(linkblock *);
extern _kernel_oserror *deletefile(linkblock *);
extern _kernel_oserror *createdir(linkblock *);
extern _kernel_oserror *createfile(linkblock *);
extern _kernel_oserror *loadfile(linkblock *);
extern _kernel_oserror *savefile(linkblock *);
extern _kernel_oserror *opensave(linkblock *);
extern _kernel_oserror *closesave(linkblock *);
extern _kernel_oserror *openload(linkblock *);
extern _kernel_oserror *closeload(linkblock *);
extern _kernel_oserror *renamex(linkblock *);
extern _kernel_oserror *create(linkblock *);
extern _kernel_oserror *convert(linkblock *);
extern _kernel_oserror *info(linkblock *);
extern _kernel_oserror *method(linkblock *);
extern _kernel_oserror *validate(linkblock *);
extern _kernel_oserror *diropen(linkblock *);
extern _kernel_oserror *archiveflush(linkblock *);

#define VENEER(name, fn) \
    _kernel_oserror *veneer_##name(linkblock *linkb) { return fn(linkb); }

VENEER(updatecat, updatecat)            /* fsentry_branchtable + 4*0 */
VENEER(loadarchive, loadarchive)        /* 1 */
VENEER(deletefile, deletefile)          /* 2 */
VENEER(createdir, createdir)            /* 3 */
VENEER(createfile, createfile)          /* 4 */
VENEER(loadfile, loadfile)              /* 5 */
VENEER(savefile, savefile)              /* 6 */
VENEER(opensave, opensave)              /* 7 */
VENEER(closesave, closesave)            /* 8 */
VENEER(openload, openload)              /* 9 */
VENEER(closeload, closeload)            /* 10 */
VENEER(rename, renamex)                 /* 11 */
VENEER(create, create)                  /* 12 */
VENEER(convert, convert)                /* 13 */
VENEER(info, info)                      /* 14 */
VENEER(method, method)                  /* 15 */
VENEER(validate, validate)              /* 16 */
VENEER(diropen, diropen)                /* 17 */
VENEER(flush, archiveflush)             /* 18 */
