; t0demo.s -- a module small enough to compile by hand.
;
; It exists to prove that ROSGD's runtime can run compiled ObjAsm, and to be
; the ObjAsm compiler's first golden test: t0demo.c beside it is this file
; translated by hand exactly as `rosasm --emit c` must translate it (tier 0).  So it exercises what tier 0 must get right:
;
;   a module header, read as data           SWIs to the kernel (native, typed)
;   a SWI to itself (compiled to compiled)  flags as results (Z from Classify)
;   an error returned with V set            a jump table (ADD pc, pc, rN)
;   fall-through between routines           an indirect call through a table
;   loads, stores, the stack, all in arena memory of code addresses
;
; SPDX-License-Identifier: MIT.  Copyright (c) 2026 Alban Read.

        AREA    |!!!Module$$Header|, CODE, READONLY

XOS_Write0       *      &20002
XOS_NewLine      *      &20003
XOS_Module       *      &2001E
XT0Demo_Classify *      &E0001          ; &C0001 with the X bit

V_bit            *      1 :SHL: 28

Module_Base
        DCD     0                       ; no start entry: not an application
        DCD     Init - Module_Base
        DCD     Final - Module_Base
        DCD     0                       ; no service call handler
        DCD     Title - Module_Base
        DCD     Help - Module_Base
        DCD     0                       ; no *commands
        DCD     &C0000                  ; SWI chunk, in the user range
        DCD     SWIHandler - Module_Base
        DCD     SWINames - Module_Base
        DCD     0                       ; no SWI decoding code
        DCD     0                       ; no messages file
        DCD     Flags - Module_Base

Flags   DCD     1                       ; 32-bit compatible

Title   =       "T0Demo", 0
Help    =       "T0Demo", 9, "0.01 (24 Sep 2026)", 0
SWINames
        =       "T0Demo", 0, "Sum", 0, "Classify", 0, "Fail", 0, "Greet", 0, "LongJump", 0, 0
        ALIGN

; ---------------------------------------------------------------------------
; Initialise: claim workspace from the RMA and hang it off the private word.
; In: R12 -> private word.
Init    STMFD   sp!, {lr}
        MOV     r0, #6                  ; OS_Module 6: claim
        MOV     r3, #16
        SWI     XOS_Module              ; R2 -> the block
        STRVC   r2, [r12]
        MOVVC   r0, #0
        STRVC   r0, [r2]                ; workspace+0 counts SWI calls
        LDMFD   sp!, {pc}

Final   MOV     pc, lr

; ---------------------------------------------------------------------------
; The SWI handler.  In: R11 = the SWI's offset in the chunk, R12 -> private
; word.  R10-R12 need not be preserved: the dispatcher does that.
SWIHandler
        LDR     r12, [r12]              ; R12 -> workspace
        LDR     r10, [r12]
        ADD     r10, r10, #1            ; count every call
        STR     r10, [r12]
        CMP     r11, #5
        ADDLO   pc, pc, r11, LSL #2     ; a jump table: one branch per SWI
        B       BadSWI
        B       Sum
        B       Classify
        B       Fail
        B       Greet
        B       LongJump
BadSWI  ADR     r0, ErrorBadSWI
        B       ReturnError

; T0Demo_Sum.  In: R0 -> words, R1 = count.  Out: R0, R1 = the 64-bit total.
Sum     MOV     r2, r0
        MOV     r0, #0
        MOVS    r3, r1                  ; the count, and Z if there are none
        MOV     r1, #0
        MOVEQ   pc, lr
SumLoop LDR     r12, [r2], #4
        ADDS    r0, r0, r12             ; the low word, C the carry out
        ADC     r1, r1, #0              ; carries into the high word
        SUBS    r3, r3, #1
        BNE     SumLoop
        MOV     pc, lr

; T0Demo_Classify.  In: R0 = a value.  Out: R0 = 0 zero, 1 negative,
; 2 below 100, 3 larger -- and Z set on exit when the value was zero.
Classify
        CMP     r0, #0
        MOVEQ   r0, #0
        MOVEQ   pc, lr                  ; Z is still set from the CMP
        MOVLT   r0, #1
        BLT     ClassifyDone
        CMP     r0, #100
        MOVLO   r0, #2
        MOVHS   r0, #3
ClassifyDone
        MOVS    r12, #1                 ; clear Z for every non-zero value
        MOV     pc, lr

; T0Demo_Fail.  Out: V set, R0 -> an error block in this module's area.
Fail    ADR     r0, ErrorDemo
ReturnError                             ; Fail falls through into here
        MSR     CPSR_f, #V_bit
        MOV     pc, lr

; T0Demo_Greet.  Out: R0 = SWI calls this module has taken, R1 = the result
; of an indirect call.  Prints through the kernel's SWIs, calls itself by
; SWI, and calls through a table of code addresses.
Greet   STMFD   sp!, {r4, lr}
        MOV     r4, r12                 ; R4 -> workspace
        ADR     r0, Message
        SWI     XOS_Write0
        SWI     XOS_NewLine
        MOV     r0, #42
        SWI     XT0Demo_Classify        ; R0 = 2: 42 is below 100
        ADR     r1, Handlers
        LDR     r1, [r1, r0, LSL #2]    ; Handlers[2] -> Twice
        MOV     r0, #21
        MOV     lr, pc
        MOV     pc, r1                  ; the indirect call: R0 = 42
        MOV     r1, r0
        LDR     r0, [r4]                ; the call count
        LDMFD   sp!, {r4, pc}

Handlers
        DCD     Once
        DCD     Once
        DCD     Twice
        DCD     Once

Once    MOV     pc, lr
Twice   ADD     r0, r0, r0
        MOV     pc, lr

; T0Demo_LongJump.  In: R0 = a value.  Out: R0 = the value plus 1, given
; from two calls deep: the sp the SWI was entered with put back and the
; entry's lr popped, over the calls' frames -- as the Wimp leaves a menu
; selection (LDR R13,longjumpSP then B ExitPoll).
LongJump
        STMFD   sp!, {lr}
        STR     sp, [r12, #4]           ; workspace+4: the sp to go back to
        BL      Deeper
        MOV     r0, #0                  ; never reached
        LDMFD   sp!, {pc}
Deeper  STMFD   sp!, {r4, lr}
        BL      Deepest
        MOV     r0, #0                  ; never reached
        LDMFD   sp!, {r4, pc}
Deepest ADD     r0, r0, #1
        LDR     sp, [r12, #4]
        LDMFD   sp!, {pc}               ; the SWI's own return

Message =       "Hello from compiled ObjAsm, running on Linux", 0
        ALIGN
ErrorDemo
        DCD     &C0000
        =       "T0Demo_Fail fails, as it should", 0
        ALIGN
ErrorBadSWI
        DCD     &1E6
        =       "T0Demo has no such SWI", 0
        ALIGN

        END
