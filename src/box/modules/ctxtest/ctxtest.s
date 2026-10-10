; ctxtest.s -- the Wimp's task switch, small: a SWI that arms the kernel's
; CallBack as ExitPoll does, and a handler that ends as callbackpoll ends.
;
; CtxTest_Switch  R0 -> the block of seventeen words to go back to
;                 R1 -> the block to leave this context's registers in
;
; Like Wimp_Poll handing over to another task, it returns at once; on the
; way out of the SWI the kernel dumps the caller's registers into R1's block
; and enters the handler, which drops into user mode with R0's (Wimp07's
; callbackpoll, whose last instructions are copied here unchanged).  The
; self-test (boot/selftest_callback.c) runs contexts through it.
;
; SPDX-License-Identifier: MIT.  Copyright (c) 2026 Alban Read.

        AREA    |!!!Module$$Header|, CODE, READONLY

XOS_ChangeEnvironment *  &20040
XOS_SetCallBack       *  &2001B
CallBackHandler       *  7

Module_Base
        DCD     0                       ; no start entry
        DCD     0                       ; no initialisation
        DCD     0                       ; no finalisation
        DCD     0                       ; no service calls
        DCD     Title - Module_Base
        DCD     Help - Module_Base
        DCD     0                       ; no *commands
        DCD     &C0040                  ; SWI chunk, in the user range
        DCD     SWIHandler - Module_Base
        DCD     SWINames - Module_Base
        DCD     0
        DCD     0
        DCD     Flags - Module_Base

Flags   DCD     1                       ; 32-bit compatible

Title   =       "CtxTest", 0
Help    =       "CtxTest", 9, "0.01 (26 Sep 2026)", 0
SWINames
        =       "CtxTest", 0, "Switch", 0, 0
        ALIGN

; In: R11 = the SWI's offset in the chunk (only Switch).
SWIHandler
        STMFD   sp!, {r0-r3, lr}
        MOV     r3, r1                  ; the buffer: this context goes here
        MOV     r2, r0                  ; the handler's R12: the one to go to
        ADR     r1, callbackpoll
        MOV     r0, #CallBackHandler
        SWI     XOS_ChangeEnvironment
        SWI     XOS_SetCallBack
        LDMFD   sp!, {r0-r3, pc}

; Entered by the kernel in SVC mode, the SVC stack empty: R12 -> the block
; of the context to go back to.
callbackpoll
        MOV     lr, r12
        TEQ     PC,PC                           ; are we 26 or 32-bit?
        LDREQ   R0,[lr,#16*4]
        MSREQ   SPSR_cxsf,R0
        LDMIA   lr,{R0-R14}^                    ; restore USR regs
        NOP
        LDR     lr,[lr,#15*4]
        MOVS    PC,lr                           ; exit to caller, restoring flags

        END
