/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* basicasm.h: the x86-64 and AArch64 instructions of BBC BASIC's inline
 * assembler (basicasm.c), and the calls that the patched ROM makes
 * (../patch-basicasm.py). */
#ifndef BASICASM_BASICASM_H
#define BASICASM_BASICASM_H

#include <stdint.h>

/* The targets of *BasicAsmCPU. */
#define BASICASM_CPU_ARM 0    /* the ROM's own ARM assembler */
#define BASICASM_CPU_X64 1    /* asmx64.c */
#define BASICASM_CPU_A64 2    /* asma64.c */

/* What basicasm_statement() found. The patched CASM acts on it. */
#define BASICASM_ARM     0    /* not an instruction: a directive, the ARM assembler's own */
#define BASICASM_WORD    1    /* an A64 word in R1: on at CASMICHK, as an ARM instruction */
#define BASICASM_BYTES   2    /* x86-64 bytes in STRACC, R2 their end: on at CASMXCHK */
#define BASICASM_ERASS1  3    /* No such mnemonic */
#define BASICASM_ERSYNT  4    /* Syntax error */
#define BASICASM_ERASS3  5    /* Bad register */
#define BASICASM_ERASS2  6    /* Bad immediate constant */
#define BASICASM_ERASS2A 7    /* Bad address offset */
#define BASICASM_ERASS2S 8    /* Bad shift */
#define BASICASM_ERTYPEINT 9  /* Type mismatch: number needed */
#define BASICASM_ERDEEPNEST 10 /* no room on BASIC's stack, as EVAL's */
#define BASICASM_STMT    11   /* an error in an operand: on at its handler, STMT */
#define BASICASM_EINVOP  12   /* Invalid arithmetic operation: a real too big */
#define BASICASM_FACERR  13   /* Unknown or missing variable: an operand missing */

/* The CPU that '[' assembles for. It belongs to the task and is set by
 * *BasicAsmCPU. It starts as the machine's own CPU (basicasm_default_cpu). */
extern _Thread_local int basicasm_cpu;
void basicasm_set_cpu(int cpu);
int basicasm_cpu_mode(void);
int basicasm_default_cpu(void);

struct ros_cpu;
/* The calls that the patched ROM makes:
 *   '[' (ASS) calls basicasm_block.
 *   A label made (CASM1A) calls basicasm_label with the address of the
 *   label's variable and its value, which is P%.
 *   An instruction statement (CASM) calls basicasm_statement.
 *   The listing of an instruction's bytes (CASMX) calls
 *   basicasm_list_bytes.
 *   ']' (CASMKET) calls basicasm_ket with the values of P% at the start
 *   and at the end of the block. */
void basicasm_block(uint32_t argp);
/* *BasicAsmCPU with no CPU. This selects the machine's CPU and turns ARM
 * program recognition on. */
void basicasm_default(void);
void basicasm_label(uint32_t var, uint32_t value);
int basicasm_statement(struct ros_cpu *s);
int basicasm_list_bytes(struct ros_cpu *s);
void basicasm_ket(struct ros_cpu *s, uint32_t start, uint32_t end);
/* BASIC's error handler is about to go on at STMT (s.Basic MSGSP1). This
 * returns to the statement that the handler abandons, if it abandons one.
 * See basicasm.c. */
void basicasm_error_goes_on(struct ros_cpu *s);
/* Called for a longjmp to the frame `to` (dispatch.c's ros_resume_unwind).
 * The statements in the frames that the longjmp leaves are gone. */
void basicasm_unwind_below(const void *to);

/* The patched ROM adds one each time a name that BASIC has no variable for
 * reads as P%. This happens with OPT's errors bit clear. It is the ARM
 * assembler's forward reference (s.Factor TSTVB1). */
extern _Thread_local unsigned basicasm_unknown;

#endif
