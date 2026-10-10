; inttest.s -- integer code, for testing the ObjAsm compiler's tier 1.
;
; The box compiles this file twice: lifted to C expressions (tier 1), and
; instruction by instruction (tier 0, --no-lift).  The self-test calls each
; routine in both with the same random registers, flags and memory, and
; every register, flag and byte must come out the same
; So the routines are the idioms tier 1
; must get right: every condition, flags set and not, carries in and out,
; compares chained, the barrel shifter, every addressing mode, loops, calls
; that return results in the flags.
;
; On entry: r0-r7 and r9-r12 are random, as are the flags; r8 points to a
; 512-byte buffer of random bytes whose byte 255 is 0.  Every access that
; runs stays inside it; GuardLd's, from random addresses, must not run.
;
; SPDX-License-Identifier: MIT.  Copyright (c) 2026 Alban Read.

        AREA    |IntTest$$Code|, CODE, READONLY

        EXPORT  Arith
        EXPORT  Shifts
        EXPORT  AddSub
        EXPORT  Logic
        EXPORT  Conds
        EXPORT  Chains
        EXPORT  FlagsIn
        EXPORT  Loops
        EXPORT  Search
        EXPORT  Memory
        EXPORT  Blocks
        EXPORT  Mults
        EXPORT  Wide
        EXPORT  Calls
        EXPORT  Switch
        EXPORT  Strlen
        EXPORT  Divide
        EXPORT  Consts
        EXPORT  Psr
        EXPORT  CondMem
        EXPORT  GuardLd
        EXPORT  Across
        EXPORT  Chain
        EXPORT  Same
        EXPORT  Idioms
        EXPORT  Names
        EXPORT  Frame
        EXPORT  Slot
        EXPORT  Outer
        EXPORT  Args

; ---------------------------------------------------------------------------
; The barrel shifter, by constants; the flags pass through.
Arith   ADD     r0, r0, r1
        SUB     r2, r2, r3, LSL #3
        RSB     r4, r5, #100
        EOR     r1, r1, r6, ROR #7
        ORR     r3, r3, r7, LSR #5
        BIC     r5, r5, r2, ASR #3
        MVN     r6, r4, LSL #1
        AND     r7, r7, r0, LSR #32
        ADD     r9, r9, r10, RRX
        MOV     r10, r11, ASR #32
        MOV     pc, lr

; Shifts by registers: any amount from 0 to 255.
Shifts  MOV     r0, r0, LSL r1
        MOV     r2, r2, LSR r3
        MOV     r4, r4, ASR r5
        MOV     r6, r6, ROR r7
        MOVS    r9, r9, LSL r10
        ADC     r11, r11, #0
        MOVS    r1, r3, LSR r5
        ADCS    r12, r12, r12
        MOV     pc, lr

; Every flag-setting arithmetic operation, carries chained.
AddSub  ADDS    r0, r0, r1
        ADCS    r2, r2, r3
        SUBS    r4, r4, r5
        SBCS    r6, r6, r7
        RSBS    r9, r9, #0
        RSCS    r10, r10, r11
        MOV     pc, lr

; The logical operations' flags, and the shifter's carry.
Logic   MOVS    r0, r0, LSR #1
        ADC     r1, r1, #0
        ANDS    r2, r2, r3, LSL #4
        ADDCS   r4, r4, #1
        ORRS    r5, r5, #&FF000000
        ADDCC   r6, r6, #1
        EORS    r7, r7, r9
        SUBMI   r10, r10, #1
        TST     r11, #&80
        MOVNE   r11, #1
        TEQ     r12, r0
        MOVEQ   r12, #2
        MOVS    r1, r1, RRX
        MOV     pc, lr

; Every condition after one compare; then a compare whose flags are the
; result.
Conds   CMP     r0, r1
        MOVEQ   r2, #1
        MOVNE   r2, #2
        MOVCS   r3, #3
        MOVCC   r3, #4
        MOVMI   r4, #5
        MOVPL   r4, #6
        MOVVS   r5, #7
        MOVVC   r5, #8
        MOVHI   r6, #9
        MOVLS   r6, #10
        MOVGE   r7, #11
        MOVLT   r7, #12
        MOVGT   r9, #13
        MOVLE   r9, #14
        CMN     r10, r11
        ADDGT   r12, r12, #1
        SUBLE   r12, r12, #1
        MOV     pc, lr

; Conditional compares: one of a set, a range, and HS then HI.
Chains  CMP     r0, #1
        CMPNE   r0, #2
        CMPNE   r0, #3
        MOVEQ   r1, #1
        MOVNE   r1, #0
        CMP     r2, #'a'
        RSBGES  r3, r2, #'z'
        MOVGE   r4, #1
        MOVLT   r4, #0
        CMP     r5, #10
        CMPCS   r6, #20
        MOVHI   r7, #1
        MOVLS   r7, #0
        MOV     pc, lr

; Conditions on the flags it was called with.
FlagsIn ADDEQ   r0, r0, #1
        SUBNE   r1, r1, #1
        MOVCS   r2, r3
        MOVMI   r4, r5
        ADDVS   r6, r6, r7
        ADCS    r9, r9, r10
        MOVHI   r11, #3
        MOVLE   r12, #4
        MOV     pc, lr

; A count down, and a count of the bits in r0.
Loops   AND     r1, r1, #31
        MOV     r2, #0
        MOVS    r3, r1
        BEQ     %FT20
10      ADD     r2, r2, r3
        SUBS    r3, r3, #1
        BNE     %BT10
20      MOV     r4, #0
        MOV     r5, r0
30      MOVS    r5, r5, LSR #1
        ADC     r4, r4, #0
        BNE     %BT30
        MOV     pc, lr

; The index of the first byte equal to r0's top byte, in the first r1 & 63
; bytes, or -1: a signed compare and conditional returns.
Search  AND     r1, r1, #63
        MOV     r2, #0
10      CMP     r2, r1
        MVNGE   r0, #0
        MOVGE   pc, lr
        LDRB    r3, [r8, r2]
        CMP     r3, r0, LSR #24
        MOVEQ   r0, r2
        MOVEQ   pc, lr
        ADD     r2, r2, #1
        B       %BT10

; Every width, and the index modes.
Memory  AND     r1, r1, #&7C
        AND     r2, r2, #&7E
        AND     r3, r3, #&7F
        LDR     r4, [r8, r1]
        LDRH    r5, [r8, r2]
        LDRSH   r6, [r8, r2]
        LDRB    r7, [r8, r3]
        LDRSB   r9, [r8, r3]
        STR     r0, [r8, #128]
        STRH    r0, [r8, #134]
        STRB    r0, [r8, #137]
        ADD     r10, r8, #64
        LDR     r11, [r10, #4]!
        STR     r11, [r10], #8
        LDR     r12, [r10, -r1, LSR #2]
        ADD     r0, r4, r5
        MOV     pc, lr

; LDM and STM in each mode, with and without writeback; a push and a pop.
Blocks  ADD     r9, r8, #192
        STMIA   r9!, {r0-r3}
        STMDB   r9, {r4, r5}
        LDMDB   r9!, {r0, r1}
        ADD     r10, r8, #160
        LDMIB   r10, {r2-r4}
        STMDA   r10, {r6, r7}
        LDMIA   r8, {r5-r7}
        STMFD   sp!, {r0-r3}
        LDMFD   sp!, {r4-r7}
        MOV     pc, lr

; Multiplies, long and short, and N and Z from one.
Mults   MUL     r0, r1, r2
        MLA     r3, r4, r5, r3
        MLS     r6, r7, r9, r6
        UMULL   r10, r11, r0, r1
        UMLAL   r10, r11, r2, r3
        SMULL   r2, r4, r5, r6
        SMLAL   r2, r4, r7, r9
        MULS    r12, r0, r12
        MOV     pc, lr

; 64-bit arithmetic in register pairs.
Wide    ADDS    r0, r0, r2
        ADC     r1, r1, r3
        SUBS    r4, r4, r6
        SBC     r5, r5, r7
        RSBS    r9, r9, #0
        RSC     r10, r10, #0
        MOVS    r11, r11, LSL #1
        ADC     r12, r12, r12
        MOV     pc, lr

; Calls to a routine whose result is the C flag.
Calls   STMFD   sp!, {r4, lr}
        MOV     r4, r0
        BL      IsSmall
        MOVCS   r1, #1
        MOVCC   r1, #0
        MOV     r0, r2
        BL      IsSmall
        ADC     r3, r3, #0
        MOV     r0, r4
        LDMFD   sp!, {r4, pc}

; Out: C set if r0 < 100; r12 corrupted.
IsSmall RSBS    r12, r0, #99
        MOV     pc, lr

; A jump table, bounded by a mask.
Switch  AND     r1, r0, #3
        ADD     pc, pc, r1, LSL #2
        MOV     r0, r0
        B       Case0
        B       Case1
        B       Case2
        B       Case3
Case0   ADD     r2, r2, #1
        MOV     pc, lr
Case1   SUB     r2, r2, #1
        MOV     pc, lr
Case2   MOVS    r2, r2, LSL #2
        MOV     pc, lr
Case3   CMP     r2, r3
        MOVHI   r2, r3
        MOV     pc, lr

; The length of the string at r8.
Strlen  MOV     r1, r8
10      LDRB    r2, [r1], #1
        CMP     r2, #0
        BNE     %BT10
        SUB     r0, r1, r8
        SUB     r0, r0, #1
        MOV     pc, lr

; Division by repeated subtraction: r0 >> 20 over r1 & 255 | 1.
Divide  AND     r1, r1, #255
        ORR     r1, r1, #1
        MOV     r0, r0, LSR #20
        MOV     r2, #0
10      CMP     r0, r1
        SUBHS   r0, r0, r1
        ADDHS   r2, r2, #1
        BHS     %BT10
        MOV     pc, lr

; Constants: MOVW and MOVT, CLZ, literal pools, a table in the image.
Consts  MOVW    r0, #&1234
        MOVT    r0, #&5678
        CLZ     r1, r2
        LDR     r3, =&DEADBEEF
        LDR     r4, =&80000000
        ADD     r5, r3, r4
        LDR     r6, Table + 4
        MOV     pc, lr
Table   DCD     1, 2, 3, 4
        LTORG

; The flags as a value.
Psr     CMP     r0, r1
        MRS     r2, CPSR
        AND     r2, r2, #&F0000000
        MOV     pc, lr

; Conditional loads whose condition never holds, from random addresses:
; run, they would fault; not run, they must not.  The loaded values are
; read more than once, which tempts a compiler to load them early.  (r0 OR
; 1 is never 0, which no compiler is asked to notice.)
GuardLd ORR     r12, r0, #1
        CMP     r12, #0
        LDREQ   r1, [r2]
        ADDEQ   r3, r1, r1
        LDMEQIA r4, {r5, r6}
        ADDEQ   r7, r5, r6
        ADDEQ   r7, r7, r5
        LDREQB  r9, [r10], #1
        ADDEQ   r11, r9, r9
        MOV     pc, lr

; Registers across a call: r4 the callee never names, r5 it reads, r6 it
; reads and writes, r3 it writes.  What the caller keeps must survive in
; place; what the callee writes must come back.
Across  STMFD   sp!, {r4-r6, lr}
        MOV     r4, r0
        ADD     r5, r1, #1
        MOV     r6, r2
        BL      Mix
        ADD     r0, r4, r5
        ADD     r1, r6, r3
        EOR     r9, r4, r6
        LDMFD   sp!, {r4-r6, pc}

; In: r5, r6.  Out: r6 = r5 + r6, r3 = r5 * 2.  Keeps the rest.
Mix     ADD     r6, r5, r6
        MOV     r3, r5, LSL #1
        MOV     pc, lr

; A call, then a tail call to the same routine: it returns to Chain's
; caller, with what Chain's caller reads.
Chain   STMFD   sp!, {lr}
        MOV     r7, r0
        BL      Twice
        ADD     r2, r7, #1
        LDMFD   sp!, {lr}
        B       Twice

; r7 doubled.
Twice   ADD     r7, r7, r7
        MOV     pc, lr

; Instructions under one condition, which the C writes as one if; and,
; between them, one that changes what the condition compared, and one
; that changes the flags it tests.  Then a load and an add joined to the
; branch they precede, and a result joined to its return.
Same    AND     r0, r0, #1
        AND     r1, r1, #1
        CMP     r0, r1
        MOVEQ   r0, #5
        ADDEQ   r1, r1, #1
        MOVEQ   r2, r0
        ANDEQS  r3, r3, #1
        MOVEQ   r4, #7
        MOVEQ   r12, #8
        TST     r5, #1
        LDRNE   r6, [r8, #8]
        ADDNE   r6, r6, #1
        BNE     %FT10
        MOV     r7, #1
10      TST     r9, #1
        MOVNE   r10, #1
        MOVNE   pc, lr
        MOV     r11, #2
        MOV     pc, lr

; Shifts in pairs -- sign and zero extension, a field, low bits cleared --
; and bits tested by shifting them into the flags.
Idioms  MOV     r1, r0, LSL #24
        MOV     r1, r1, ASR #24
        MOV     r2, r0, LSL #16
        MOV     r2, r2, ASR #16
        MOV     r3, r0, LSL #24
        MOV     r3, r3, LSR #24
        MOV     r4, r0, LSL #20
        MOV     r4, r4, LSR #28
        MOV     r5, r0, LSR #2
        MOV     r5, r5, LSL #2
        MOVS    r6, r9, LSL #31
        ADDMI   r7, r7, #1
        MOVS    r6, r10, LSL #24
        ADDEQ   r11, r11, #1
        MOVS    r6, r12, LSR #8
        SUBNE   r11, r11, #1
        MOVS    r6, r12, ASR #4
        ADDEQ   r11, r11, #2
        TST     r9, #&300
        ORRNE   r7, r7, #&80000000
        BIC     r10, r10, #3
        MOV     pc, lr

; Constants by name: flags, and a storage map on r8 -- a C struct -- with
; a word at offset 0, a byte, and an array a word is read from; and a
; literal.
Flag_A  *       &10
Flag_B  *       4
        ^       0, r8
Rec_First       #       4
Rec_Second      #       4
Rec_Byte        #       1
Rec_Pad         #       3
Rec_Name        #       8

Names   TST     r0, #Flag_A
        ORRNE   r1, r1, #Flag_B
        LDR     r2, Rec_First
        LDR     r3, Rec_Second
        ADD     r4, r4, #Flag_B
        CMP     r5, #Flag_A
        MOVEQ   r6, #Flag_B
        LDR     r7, =Flag_A
        LDRB    r9, Rec_Byte
        LDR     r10, Rec_Name
        ADD     r11, r8, #Rec_Name
        STR     r0, Rec_Second
        STRB    r1, Rec_Byte
        LDR     r12, Rec_First
        BIC     r12, r12, #Flag_A
        STR     r12, Rec_First
        MOV     pc, lr
        LTORG

; A call to a routine with a frame: it saves what it uses and puts it
; back, one way out popping and returning on a condition -- sp comes back
; as it went on both.
Frame   STMFD   sp!, {r4, lr}
        MOV     r4, r1
        BL      Keep
        ADD     r0, r0, r4
        LDMFD   sp!, {r4, pc}

; In: r0, r2.  Out: r0 = r0 + r2, or r2 where r0 is 0.
Keep    STMFD   sp!, {r4, r5, lr}
        MOVS    r4, r0
        LDMEQFD sp!, {r4, r5, lr}
        MOVEQ   r0, r2
        MOVEQ   pc, lr
        ADD     r5, r4, r2
        MOV     r0, r5
        LDMFD   sp!, {r4, r5, pc}

; A saved register replaced before the pop: Swap's r0 comes back as r2,
; written where the pop takes r0 from.
Slot    STMFD   sp!, {r0, lr}
        BL      Swap
        ADD     r1, r1, r0
        LDMFD   sp!, {r0, pc}

Swap    STMFD   sp!, {r0, lr}
        STR     r2, [sp]
        LDMFD   sp!, {r0, pc}

; A callee that writes into its caller's frame: Clobber's store lands on
; Poke's saved r4, so Poke gives back r3 in r4, not what it was given.
Outer   STMFD   sp!, {lr}
        BL      Poke
        ADD     r0, r4, #1
        LDMFD   sp!, {pc}

Poke    STMFD   sp!, {r4, lr}
        MOV     r4, #7
        BL      Clobber
        LDMFD   sp!, {r4, pc}

Clobber STR     r3, [sp]
        MOV     pc, lr

; A callee that reads its caller's frame, as a stack argument is read:
; Args's saved r9 is what Stacked loads, so Args's frame stays memory.
Args    STMFD   sp!, {r9, lr}
        BL      Stacked
        LDMFD   sp!, {r9, pc}

Stacked LDR     r0, [sp]
        MOV     pc, lr

; Conditional loads and stores with writeback.
CondMem AND     r1, r1, #&3C
        CMP     r0, #0
        STRNE   r2, [r8, r1]!
        LDREQ   r3, [r8, #4]
        LDRNE   r4, [r8], #4
        STRGT   r5, [r8, #-4]
        MOV     pc, lr

        END
