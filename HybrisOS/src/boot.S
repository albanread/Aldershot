/* boot.S: the HAL's first instructions.
 *
 * QEMU's virt machine loads this image in the same way as it loads a Linux
 * kernel. It places it at the base of RAM (&40000000) plus text_offset, and
 * enters it with the device tree's physical address in x0 and the MMU off.
 * It enters at EL1 (under HVF) or at EL2 (under TCG with virtualization
 * turned on). The HAL is linked to run in the top half of the address
 * space, at HAL_KOFF plus its physical address, so this code:
 *
 *   1. drops to EL1 if it was entered at EL2;
 *   2. checks that it was loaded where it is linked to run;
 *   3. builds one level-0 and one level-1 page table. These hold 1 GB
 *      blocks that map the first HAL_MAP_GB GB of physical memory twice:
 *      where it is (through TTBR0, so that the next instruction still
 *      runs) and at HAL_KOFF (through TTBR1);
 *   4. turns the MMU on, and branches to hal_main at its high address.
 *
 * hal_main replaces TTBR0 at once. The low half of the address space
 * belongs to the box (the arena and /init), and never to the HAL. */
#include "hal.h"

    .section .text.boot, "ax"
    .globl _start
_start:
    b       start                       /* code0 */
    .long   0                           /* code1 */
    .quad   HAL_TEXT_OFFSET             /* text_offset */
    .quad   _end - _start               /* image_size */
    .quad   2                           /* flags: little-endian, 4 KB pages */
    .quad   0, 0, 0                     /* res2-res4 */
    .ascii  "ARM\x64"                   /* magic */
    .long   0                           /* res5 */

start:
    mov     x19, x0                     /* keep the device tree's address */

    /* Drop from EL2 to EL1, if necessary */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    1f
    mov     x0, #(1 << 31)              /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5                  /* EL1h, DAIF masked */
    msr     spsr_el2, x0
    adr     x0, 1f
    msr     elr_el2, x0
    eret
1:
    /* Check that the image was loaded where it was linked to run. (A
     * different text_offset would need a different link address.) */
    adr     x0, _start
    ldr     x1, =(HAL_LOAD_PA)
    cmp     x0, x1
    b.ne    wrong_place

    /* Set up a stack, by physical address, for the C code called below */
    adrp    x0, boot_stack_top
    add     x0, x0, :lo12:boot_stack_top
    mov     sp, x0

    /* Zero the BSS (by physical address, because the MMU is off) */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
2:  cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b
3:
    /* Build the boot page tables. l0[0] points to l1, and l1[i] is a 1 GB
     * block at i GB. */
    adrp    x0, boot_l0
    add     x0, x0, :lo12:boot_l0
    adrp    x1, boot_l1
    add     x1, x1, :lo12:boot_l1
    orr     x2, x1, #3                  /* table descriptor */
    str     x2, [x0]
    mov     x3, #0                      /* i */
4:  lsl     x4, x3, #30                 /* physical address */
    cmp     x3, #1
    b.hs    5f
    ldr     x5, =HAL_BLOCK_DEVICE       /* the first GB holds devices */
    b       6f
5:  ldr     x5, =HAL_BLOCK_NORMAL
6:  orr     x4, x4, x5
    str     x4, [x1, x3, lsl #3]
    add     x3, x3, #1
    cmp     x3, #HAL_MAP_GB
    b.lo    4b

    /* Set up the MMU registers */
    ldr     x0, =HAL_MAIR
    msr     mair_el1, x0
    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #7                  /* copy PARange into TCR.IPS */
    ldr     x0, =HAL_TCR
    orr     x0, x0, x1, lsl #32
    msr     tcr_el1, x0
    adrp    x0, boot_l0
    add     x0, x0, :lo12:boot_l0
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x0
    /* Enable floating point and SIMD for EL1 and EL0, because /init uses them */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x0, sctlr_el1
    /* Set M, C and I (the MMU and both caches). For EL0's programs, set
     * DZE (so that musl's memset may use dc zva), UCT (so that they may
     * read the cache type) and UCI (so that they may do cache
     * maintenance). Set SPAN, so that PAN is not set on taking an
     * exception and the HAL may reach EL0's pages. */
    ldr     x1, =((1 << 0) | (1 << 2) | (1 << 12) | (1 << 14) | (1 << 15) | (1 << 23) | (1 << 26))
    orr     x0, x0, x1
    bic     x0, x0, #(1 << 1)           /* no alignment checking: musl's
                                           string routines make unaligned reads */
    msr     sctlr_el1, x0
    isb

    /* Branch to the high half of the address space */
    ldr     x0, =high
    br      x0
high:
    ldr     x0, =boot_stack_top
    mov     sp, x0
    mov     x0, x19                     /* the device tree's physical address */
    bl      hal_main
7:  wfe
    b       7b

/* A core started by PSCI CPU_ON (see smp.c) enters here with its number in
 * x0 and the MMU off. This code sets up its stack, loads the boot core's
 * page tables and MMU registers, branches to the high half of the address
 * space, and then calls the C code. */
    .globl secondary_start
secondary_start:
    mov     x19, x0
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    11f
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, 11f
    msr     elr_el2, x0
    eret
11:
    adrp    x1, stacks
    add     x1, x1, :lo12:stacks
    mov     x2, #32768
    add     x3, x19, #1
    madd    x1, x2, x3, x1                  /* the top of stacks[id] */
    mov     sp, x1
    ldr     x0, =HAL_MAIR
    msr     mair_el1, x0
    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #7
    ldr     x0, =HAL_TCR
    orr     x0, x0, x1, lsl #32
    msr     tcr_el1, x0
    adrp    x0, boot_l0
    add     x0, x0, :lo12:boot_l0
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x0
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x0, sctlr_el1
    ldr     x1, =((1 << 0) | (1 << 2) | (1 << 12) | (1 << 14) | (1 << 15) | (1 << 23) | (1 << 26))
    orr     x0, x0, x1
    bic     x0, x0, #(1 << 1)
    msr     sctlr_el1, x0
    isb
    ldr     x0, =12f
    br      x0
12:
    mov     x1, sp
    ldr     x2, =HAL_KOFF
    add     x1, x1, x2
    mov     sp, x1
    mov     x0, x19
    bl      secondary_main
13: wfe
    b       13b

wrong_place:
    /* Write the message to the PL011 at its physical address, as the MMU is off */
    ldr     x1, =HAL_UART_PA
    adr     x2, wrong_msg
8:  ldrb    w3, [x2], #1
    cbz     w3, 7b
    str     w3, [x1]
    b       8b
wrong_msg:
    .asciz  "HAL: not loaded at the address it is linked for (text_offset)\r\n"

    .ltorg

    .section .bss
    .balign 4096
    .globl boot_l0
boot_l0:  .space 4096
boot_l1:  .space 4096
boot_stack: .space 65536
    .globl boot_stack_top
boot_stack_top:
