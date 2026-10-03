# BBC BASIC in Translation

*3 October 2026*

## Introduction

BBC BASIC is part of RISC OS, and BOX would not be RISC OS without it.
BOX's BASIC is RISC OS 5.31's own BASICVFP, the build of BBC BASIC V that
uses 8-byte floating point, translated from its ObjAsm source to C.
`*BASIC` starts it, and it runs ordinary BASIC programs as they are.

This article describes:

* how BASIC is translated, and where it lives
* how it runs a BASIC program
* why it is not especially fast, and why it takes a lot of memory
* the inline assembler, which is the one real change we have made to it,
  and why it is provided out of tradition rather than recommended.

Timings throughout were measured on 3 October 2026 on the Apple silicon
box (two virtual CPUs of an M4 Max), unless stated otherwise.

---

## 1. A tier 1 translation

BASIC is translated, not rewritten. Its behaviour *is* its
specification: the exact digits `PRINT` gives, the order errors are
found in, and a good many quirks that programs have come to rely on are
written down nowhere but in the source. Rewriting it from documents
would lose some of them.

ROSASM compiles the 23,806 lines of ObjAsm in `Sources/Programmer/BASIC`
into 51,826 lines of tier 1 C (see *Tiers of Translation*): registers
held in C local variables, flags turned into comparisons where they can
be, and loops and conditions in place of branches. The author's comments
come across beside the code they describe. The only change made to the
translated interpreter is the assembler (section 6).

The translation is checked against RISC OS 5.30 itself. Seven test
programs (number formatting, the language, errors, files and `SYS`,
floating point exceptions, and some 1,600 values of the elementary
functions) are run in BOX and on real RISC OS 5.30, and every line they
print must be the same, blank lines and trailing spaces included. The
163 programs of the Rosetta Code corpus are compared in the same way.

BASICVFP is the only BASIC in the ROM. `*BASIC` is an alias for it.
BASIC105, the 5-byte BASIC V, is not built, although the same script
can still make it.

---

## 2. Where it lives

BASIC is in two places, as all translated modules are.

| What | Where | Size |
| --- | --- | --- |
| The ROM image: module header, tables, messages, constants | the arena, at `&FC100000` | 66,076 bytes |
| The compiled code | `/init`, above 4 GB | 478,016 bytes of A64 code, 76,400 of read-only data |
| A program, its variables and its stack | the application slot, from `&8000` | as on RISC OS |

The ROM image is laid out exactly as ObjAsm laid it out, so a program
that reads BASIC's tables, or a `*Help` that reads its module header,
finds them where it expects. The code is ordinary 64-bit host code
inside `/init`, out of reach of any RISC OS address. It reads and
writes the arena through the runtime's accessors.

A BASIC program is held in the application slot, as on RISC OS: the
program at `PAGE`, then its variables, then free space up to the stack
below `HIMEM`. Reals are 8-byte doubles, stored in the arena in the
same format as on RISC OS.

---

## 3. How it runs a program

Running a BASIC program in BOX is the same as on RISC OS. The program is
tokenised, either when loaded as text or already on disc, and the
interpreter walks the tokens one statement at a time. The interpreter is
the original, only compiled for a different processor, so it does every
step the original does, in the same order: the same scans for `ENDPROC`
and `NEXT`, the same variable lookup by first letter, the same error
handling and the same results.

When BASIC calls RISC OS, through `SYS`, a `*` command or a keyword such
as `PRINT` or `OPENIN`, the call is a direct C function call into the SWI
dispatcher on the task's own thread. No trap or processor exception is
involved.

---

## 4. Why it is not especially fast

The interpreter is a tier 1 translation of an interpreter. Each of those
two things costs time.

An interpreter does a lot of work for each statement: it reads tokens,
looks up variables, checks types, and converts between integers and
reals. On RISC OS that work was done by hand-tuned ARM code. In BOX it is
done by the same code translated, which keeps the assembler's shape:

* registers are written back to the register block around every call
  and SWI, so they can be seen there;
* the translated BASIC has 1,586 checked returns, because the assembler
  returns through saved link registers and the C must check where each
  return goes;
* 749 `setjmp` resume points reproduce the assembler's non-local exits
  (errors, `ENDPROC`, `ON ERROR`), and each forces live values out to the
  stack;
* flags are still stored where a later instruction might read them.

None of this is a fault to be tuned out. It is the price of keeping
BASIC's behaviour exactly.

### Mandelbrot timings

The Mandelbrot kernel from the `MandelA64` example computes 320 × 256
points, at most 64 iterations each, in BASIC's floating point. Run four
ways, each giving the same 81,920 results:

| How it is run | Time | Relative to BOX's interpreter |
| --- | --- | --- |
| RISC OS 5.30 BASICVFP on an emulated Raspberry Pi 4 (QEMU TCG, same Mac) | 2.11 – 2.18 s | 2.5 × slower |
| BOX's BASIC, interpreted | 0.86 s | 1 |
| The same program compiled by ROSBAS | 0.0235 s | 37 × faster |
| The kernel in A64 code, by BASIC's own assembler | 0.0030 s | 290 × faster |

The interpreted program is about two and a half times as fast as the
real BASIC running under emulation on the same machine, so it is not
slow. But compiling the same unchanged source with `*RosBas` makes it
37 times faster again. For speed, compile.

(The ROSBAS and A64 times are each the average of 100 runs, as both are
below the centisecond resolution of `TIME`.)

---

## 5. Why it takes a lot of memory

The ARM interpreter is 66 KB. Its translation is 478 KB of code and
76 KB of read-only data, about eight times as much. The ROM image is
still needed as well, for the tables and messages that programs read.

The growth comes from the same source as the lost speed. One ARM
instruction often does several things at once: a conditional load with
writeback, a shifted operand, and a flag update. Written out in C, and
compiled for another processor, each of those becomes separate
instructions. Every checked return, resume point and register spill
listed above adds code of its own.

The memory a BASIC program uses for itself is the same as on RISC OS.

---

## 6. The inline assembler

The one real change to BASIC is its assembler. On RISC OS,
`[ ... ]` assembles ARM code into memory and `CALL` or `USR` runs it.
BOX has no ARM processor, so ARM code assembled this way can never run.

BOX's BASIC instead assembles code for the processor it is running on:
A64 on the Apple silicon box, x86-64 on Intel. This is the default.
`*BasicAsmCPU A64`, `X64` or `ARM` selects one explicitly, for cross
assembly. The encoders are a library added to BASICVFP's ROM, and are
checked against LLVM's assembler instruction by instruction.

Programs use it in the traditional way, with two passes:

```
  570 FOR pass%=0 TO 2 STEP 2
  580 P%=code%
  590 [OPT pass%
  600 ldr w9, [x0]
  610 ldr w10, [x0, #4]
  ...
  920 fmul d4, d0, d0
  930 fmul d5, d1, d1
  940 fsub d4, d4, d5
  ...
```

### The contract

* **Entry.** The code is called as an ordinary function of the host
  (AAPCS64 on A64, SysV on x86-64). The first argument (`x0` or `rdi`)
  is the arena address of the register block: R0 to R15, then the PSR,
  as words. `CALL` loads R0 to R7 from `A%` to `H%`; `USR` returns R0.
* **Memory.** A BASIC address is a host address. `DIM`'d blocks,
  strings and variables can be used directly.
* **Registers.** The host's rules apply: callee-saved registers must be
  preserved. On A64, `x18` belongs to the runtime; on x86-64, `%gs`.
* **SWIs.** Through the SWI gate at `&FEEFF000`.
* **Faults.** On the Apple silicon box, a data abort in assembled code
  becomes a RISC OS error, which `ON ERROR` can trap.

When a block ends at `]`, BASIC tells the runtime where the new code
is, so that `CALL` will accept it. Code assembled into the application
slot is forgotten when a new application starts.

### Why it is not recommended

The assembler is there because BBC BASIC has always had one, and
because people enjoy it. We do not recommend it for new work:

* **The code is tied to one processor.** A64 code will not run on the
  Intel box, nor x86-64 code on Apple silicon. A program that assembles
  must carry a kernel for each, as the examples do (`MandelA64`,
  `MandelX64`).
* **ARM code is gone.** An existing program that assembles ARM code
  stops at its first ARM instruction, because the default is the host's
  processor. With `*BasicAsmCPU ARM` it assembles, but the code cannot
  run. Such programs have to be rewritten whatever happens.
* **The dialect is limited.** The assembler's expressions are evaluated
  in integers, with numbers, labels, BASIC variables, `+ - * /` and
  brackets, but not BASIC's functions or other operators. Labels are the
  assembler's own, not BASIC variables.
* **Compiling gets most of the way.** The ROSBAS-compiled program above
  runs in 23.5 ms against the assembler's 3.0 ms, on every processor,
  from plain BASIC that anyone can read. Where that is not enough, write
  the routine in C with `*CC`.

The assembler is a link with BBC BASIC's past, and it works. For a
program you want to keep, write BASIC and compile it, or write C.
