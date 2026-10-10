; fptest.s -- floating point, for the ObjAsm compiler to lift.
;
; Two SWIs compute the same things, one in FPA and one in VFP, so that
; rosasm --emit c has real floating point to turn into C expressions
; (standard C floating point, no emulator).  The self-test checks every result, bit for bit, against the
; same computation written in C.
;
; Block layout (R0 on entry): inputs a, b, c at 0, 8 and 16; results from 32.
; FPA's doubles are high word first, VFP's low word first.
;
; SPDX-License-Identifier: MIT.  Copyright (c) 2026 Alban Read.

        AREA    |!!!Module$$Header|, CODE, READONLY

V_bit   *       1 :SHL: 28

Module_Base
        DCD     0
        DCD     Init - Module_Base
        DCD     0
        DCD     0
        DCD     Title - Module_Base
        DCD     Help - Module_Base
        DCD     0
        DCD     &C0080
        DCD     SWIHandler - Module_Base
        DCD     SWINames - Module_Base
        DCD     0
        DCD     0
        DCD     Flags - Module_Base

Flags   DCD     1
Title   =       "FPTest", 0
Help    =       "FPTest", 9, "0.01 (24 Sep 2026)", 0
SWINames
        =       "FPTest", 0, "FPA", 0, "VFP", 0, 0
        ALIGN

Init    MOV     pc, lr

SWIHandler
        CMP     r11, #2
        ADDLO   pc, pc, r11, LSL #2
        B       BadSWI
        B       FPA
        B       VFP
BadSWI  ADR     r0, ErrorBadSWI
        MSR     CPSR_f, #V_bit
        MOV     pc, lr

; ---------------------------------------------------------------------------
; FPTest_FPA.  Out: R0 = (a * 10) * 10, truncated.
FPA     STMFD   sp!, {lr}
        SFM     f4, 4, [sp, #-48]!      ; keep f4-f7, as APCS has it
        LDFD    f0, [r0]
        LDFD    f1, [r0, #8]
        MUFD    f2, f0, f0
        MUFD    f3, f1, f1
        ADFD    f2, f2, f3
        SQTD    f2, f2
        STFD    f2, [r0, #32]           ; hypot(a, b)
        SIND    f4, f0
        COSD    f5, f1
        MUFD    f4, f4, f5
        LDFD    f5, Third
        ADFD    f4, f4, f5
        STFD    f4, [r0, #40]           ; sin(a) * cos(b) + 1/3
        POLD    f6, f0, f1
        STFD    f6, [r0, #48]           ; atan2(b, a)
        LDFD    f7, [r0, #16]
        CMF     f7, #0
        MVFLTD  f7, #0
        STFD    f7, [r0, #56]           ; c, or 0 if below 0 or unordered
        CMP     r0, r0                  ; the compare's flags end here
        ADFS    f3, f0, f1
        STFS    f3, [r0, #64]           ; a + b, rounded to single
        MUFD    f1, f0, #10
        MUFD    f1, f1, #10
        FIXZ    r0, f1
        LFM     f4, 4, [sp], #48
        LDMFD   sp!, {pc}

Third   DCD     &3FD55555, &55555555    ; 1/3, high word first

; ---------------------------------------------------------------------------
; FPTest_VFP.  Out: R0 = a, truncated.
VFP     VLDR    d0, [r0]
        VLDR    d1, [r0, #8]
        VMUL.F64 d2, d0, d0
        VMLA.F64 d2, d1, d1
        VSQRT.F64 d2, d2
        VSTR    d2, [r0, #32]           ; hypot(a, b)
        VMOV.F64 d3, #1.0
        VFMA.F64 d3, d0, d1
        VSTR    d3, [r0, #40]           ; fma(a, b, 1)
        VPUSH   {d8}
        VMOV.F64 d8, #2.0
        VMUL.F64 d5, d0, d8
        VSTR    d5, [r0, #48]           ; a * 2
        VPOP    {d8}
        VLDR    d4, [r0, #16]
        VCMP.F64 d4, #0
        VMRS    APSR_nzcv, FPSCR
        BGE     %FT10
        MOV     r1, #0
        VMOV    d4, r1, r1
10      VSTR    d4, [r0, #56]           ; c, or 0 if below 0 or unordered
        CMP     r0, r0                  ; the compare's flags end here
        VCVT.F32.F64 s12, d0
        VCVT.F32.F64 s13, d1
        VADD.F32 s12, s12, s13
        VSTR    s12, [r0, #64]          ; (float)a + (float)b, in single
        VCVT.S32.F64 s0, d0
        VMOV    r0, s0
        MOV     pc, lr

ErrorBadSWI
        DCD     &1E6
        =       "FPTest has no such SWI", 0
        ALIGN

        END
