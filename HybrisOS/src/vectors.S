/* vectors.S: the exception vectors, and the code that enters the box.
 *
 * /init's own code runs at EL1 using SP_EL0 ("EL1t"), so its system calls
 * and faults arrive at the "current EL, SP0" vectors. Programs, and RISC OS
 * applications inside /init, run at EL0 (since phase H6), so theirs arrive at
 * the "lower EL" vectors. The HAL's own exceptions run using SP_EL1
 * ("EL1h"). Every entry saves the whole
 * register frame (struct frame in hal.h) and calls trap(frame, kind). The
 * interrupted code continues with whatever trap leaves in the frame. */

#define FRAME 288                       /* sizeof(struct frame) */

    .macro entry kind
    .balign 0x80
    sub     sp, sp, #FRAME
    stp     x0, x1, [sp, #0]
    mov     x0, #\kind
    b       save
    .endm

    .section .text
    .balign 2048
    .globl vectors
vectors:
    entry 0                             /* current EL, SP0: synchronous */
    entry 1                             /*   IRQ */
    entry 2                             /*   FIQ */
    entry 3                             /*   SError */
    entry 4                             /* current EL, SPx: synchronous */
    entry 5
    entry 6
    entry 7
    entry 8                             /* lower EL, AArch64 (phase H6) */
    entry 9
    entry 10
    entry 11
    entry 12                            /* lower EL, AArch32: never used */
    entry 13
    entry 14
    entry 15

save:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x2, sp_el0
    stp     x30, x2, [sp, #240]
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x2, x3, [sp, #256]
    mrs     x2, esr_el1
    mrs     x3, far_el1
    stp     x2, x3, [sp, #272]
    mov     x1, x0                      /* kind */
    mov     x0, sp                      /* the frame */
    bl      trap
    /* drop through into restore */
    .globl restore
restore:
    ldp     x2, x3, [sp, #256]
    msr     elr_el1, x2
    msr     spsr_el1, x3
    ldp     x30, x2, [sp, #240]
    msr     sp_el0, x2
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #FRAME
    eret

/* resume_frame(f, top) copies frame f to the top of this core's stack
 * (whose end is top), then restores it and returns to it. smp.c uses this
 * to start a core's first thread. The frame is copied here because the C
 * code that built it cannot build it in place: that code has its own stack
 * frame at the top of the stack. */
    .globl resume_frame
resume_frame:
    sub     x2, x1, #FRAME
    mov     x3, #0
1:  ldr     x4, [x0, x3]
    str     x4, [x2, x3]
    add     x3, x3, #8
    cmp     x3, #FRAME
    b.lo    1b
    mov     sp, x2
    b       restore

/* hal_enter_box(entry, sp) starts the box at entry, on stack sp, at EL1
 * using SP_EL0, with every register 0. The box runs with interrupts
 * enabled (for the tick). The HAL, which an exception enters, runs with
 * them disabled. */
    .globl hal_enter_box
hal_enter_box:
    msr     sp_el0, x1
    msr     elr_el1, x0
    mov     x2, #0x344                  /* EL1t, with D, A and F masked and IRQs enabled */
    msr     spsr_el1, x2
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x29, #0
    mov     x30, #0
    eret
