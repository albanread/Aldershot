# Tiers of Translation

*8 October 2026*

## Introduction

Most of RISC OS is written in ARM assembler. BOX runs no ARM code, so
every part of RISC OS that BOX uses has to become C in one way or
another. There is more than one way of doing that, and the results
differ a great deal in accuracy, speed and how easy the code is to
change afterwards.

This article describes the four levels, or *tiers*, that BOX uses, and
what each is for.

---

## 1. The four tiers

Two questions separate them. Who writes the code: a machine, or a
person? And what decides whether it is right — what is the *oracle*
against which it is tested?

| Tier | Written by | Made from | Oracle |
| --- | --- | --- | --- |
| 0 | the compiler | the ARM instructions, one at a time | the ARM architecture itself: exact by construction |
| 1 | the compiler | tier 0, lifted into structured C | tier 0, by differential test |
| 2 | a person | a specification of what the module does | the specification, and RISC OS 5.30's behaviour |
| 3 | a person | the translation of the original code | the translation, and RISC OS 5.30's own output |

Tiers 0 and 1 are *translation*: the machine does the work, and the
original code is the authority. Tiers 2 and 3 are *rewriting*: a person
does the work, and the two differ in what that person is allowed to work
from.

That last distinction is the important one, and it is easy to miss.

* A **tier 2** rewrite is written from a specification — the PRMs, the
  module's documentation, observed behaviour. The code being replaced
  may not even have been read. The rewrite can therefore be better
  organised than the original, but it can only be as accurate as the
  specification, and specifications have gaps.
* A **tier 3** rewrite is written from the translation, function by
  function, with the translation left in place beside it. Every function
  the rewrite replaces can be run both ways on the same inputs and the
  answers compared. The oracle is the original code's own behaviour, so
  the accuracy is that of tier 0 — but the code is written by a person
  and reads like C.

Tier 3 therefore buys tier 2's speed and readability without giving up
tier 0's accuracy. It costs more work, and it is only possible where a
translation already exists to be tested against.

### The names in the code

The tiers are not just a way of talking. They appear in the build, and
the documents and the code use them the same way:

| Where | What it means |
| --- | --- |
| `rosasm --emit c --no-lift` | produce tier 0 |
| `rosasm --emit c` | produce tier 1 (the default) |
| `make BASIC=lift` | build the ROM's BASIC from the tier 1 translation |
| `make BASIC=tier3` | build it from the hand translation (the default) |
| `modules/basicvfp/tier3/units/` | the hand-written tier 3 functions |

### Readability is not a tier

An earlier version of this article counted "made readable" as a tier of
its own, above tier 2. That was a mistake, and it has been dropped.
Readability is a property that every tier has to some degree — it is a
column in the table below, not a row — and treating it as a tier left no
name for the thing BOX actually does to get its best code, which is to
rewrite by hand against a translation. That is what tier 3 now means,
and what the build has always called it.

With that settled, the four tiers compare like this:

| Tier | Name | Accuracy | Speed | Readability |
| --- | --- | --- | --- | --- |
| 0 | Unlifted | exact | poor | very poor |
| 1 | Lifted | exact, if the lifter is right | fair | fair |
| 2 | Rewritten from a specification | as good as the specification | good | good |
| 3 | Rewritten against the translation | exact, by test | good | good |

The examples below use the `Sum` routine from T0Demo, a small test
module, which adds up a block of words into a 64-bit total:

```
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
```

---

## 2. Tier 0: unlifted code

Tier 0 is what you get if you take a JIT compiler and run it ahead of
time. Each ARM instruction becomes one C statement, working on a block
of registers held in memory. Flags are fields in the same block.
Branches are labels and `goto`s. The guest stack is ordinary arena
memory, reached through R13.

This is the tier 0 output for `Sum`, exactly as the compiler writes it:

```c
Sum:
    R[2] = R[0];                                /* MOV r2, r0 */
    R[0] = 0;                                   /* MOV r0, #0 */
    R[3] = ros_logic(s, R[1], s->c);            /* MOVS r3, r1 */
    R[1] = 0;                                   /* MOV r1, #0 */
    if (s->z) {                                 /* MOVEQ pc, lr */
        R[15] = R[14];
        return;
    }
SumLoop:
    R[12] = ros_ld32(R[2]);                     /* LDR r12, [r2], #4 */
    R[2] += 4;
    R[0] = ros_adds(s, R[0], R[12]);            /* ADDS r0, r0, r12 */
    R[1] = R[1] + 0 + s->c;                     /* ADC r1, r1, #0 */
    R[3] = ros_subs(s, R[3], 1);                /* SUBS r3, r3, #1 */
    if (!s->z) goto SumLoop;                    /* BNE SumLoop */
    R[15] = R[14];                              /* MOV pc, lr */
    return;
```

### Accuracy

Tier 0 is exact by construction. The ARM Architecture Reference Manual
defines what every instruction does, and the compiler does exactly that,
one instruction at a time. Nothing is guessed. An instruction the
compiler cannot model is reported as an error at that source line.

It follows that tier 0 code is only as good as the translator. **Every
bug in tier 0 code is a translator bug.** If it goes wrong, either the
decoder or the assembler front end has got an instruction wrong. Two
such bugs were found in rosasm's FPA handling: every FPA compare was
counted as two words, so every label after one was four bytes out
(SharedCLibrary was among the victims), and post-indexed transfers such
as `LFM f4, 4, [sp], #48` lost their offset and writeback. Neither was a
fault in the RISC OS code being translated.

### Speed

Tier 0 compiles badly. The registers and flags live in a block in
memory, and every arena load might alias that block, so the C compiler
cannot keep any of them in machine registers. Each instruction becomes a
load and a store. The summing loop above takes **2.3 ns per word**.

### Readability

It is assembler written in C. The original author's comments are kept
beside each statement, which helps, but you cannot follow the logic of a
large routine without the ObjAsm open beside it.

### What it is for

Tier 0 is the reference. Every other tier is tested against it.

---

## 3. Tier 1: lifted code

Lifting is the compiler's next step. It is still automatic, and the code
is still partly an emulation of the machine, but it now looks like C.

* Registers become C local variables (`r0`, `r1` and so on), and only go
  back to the register block where a call, a SWI or a return needs them.
* Flags that are only tested become ordinary C comparisons. A `CMP`
  followed by `MOVGE` becomes `(int32_t)R[2] >= 97`.
* Labels and `goto`s are turned into `if`, `while`, `do ... while`,
  `for` and `switch`, using the method of Ramsey's "Beyond Relooper".
* Constants and workspace offsets keep their names from the source, and
  workspace maps become C structures.

The same routine, lifted:

```c
r2 = r0;                                /* MOV r2, r0 */
r0 = 0;                                 /* MOV r0, #0 */
r3 = ros_logic(s, r1, s->c);            /* MOVS r3, r1 */
r1 = 0;                                 /* MOV r1, #0 */
if (r3 == 0) {                          /* MOVEQ pc, lr */
    R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[10] = r10; R[12] = r12;
    R[15] = r14;
    return;
}
do {
    r12 = ros_ld32(r2);                 /* LDR r12, [r2], #4 */
    r2 += 4;
    r0 += r12;                          /* ADDS r0, r0, r12 */
    r1 += (r0 < r12);                   /* ADC r1, r1, #0 */
    r3 = ros_subs(s, r3, 1);            /* SUBS r3, r3, #1 */
} while (r3 != 0);                      /* BNE SumLoop */
```

The loop is now a loop, and the carry is written as a carry. The traces
of the machine are still there: the registers keep their ARM names, the
`SUBS` still sets the flags in the block because the compiler cannot
prove nothing outside reads them, and the registers are written back to
`R[]` on the way out.

### Accuracy

Lifting is a set of transformations, and each can be wrong, so tier 1 is
tested against tier 0. The IntTest module is compiled both ways and its
routines are run in both with the same random registers, flags and
memory, 3,000 inputs per routine. Every register, flag and byte must
match. This has found real lifter bugs. In one, a conditional load was
moved out from under its condition, so it would have read an address the
original never touched. In another, a register written only inside a
loop was returned unset when the loop ran zero times.

Lifting can also show up bugs in the original. MakePSFont's `get_angle`
lifted to a single line of C, and in that form it was plain that two
constants had been swapped: a 12° italic came out as 0.37°. The fault
had been there all along; one statement per instruction had hidden it.

### Speed

With registers in locals the summing loop takes **1.1 to 1.4 ns per
word**, about twice as fast as tier 0. Across the 135 corpus units that
compile, labels fall from 618 to 110 and `goto`s from 821 to 188.

Large modules show the limits. The translated Window Manager is 103,268
lines of C from 64K lines of ObjAsm. Its object code is five to six
times the size of the ARM original. It spills and reloads registers
around every call and SWI (about 29,000 places), stores the flags at
about 2,000 compares, and needs 1,258 `setjmp` resume points to
reproduce the assembler's non-local returns. None of this is a fault to
be tuned out. It is what it costs to keep the assembler's structure.

### Readability

You can read a tier 1 routine on its own, and follow its loops and
conditions. You cannot easily change it, because it still thinks in
registers.

---

## 4. Tier 2: rewritten from a specification

Tier 2 code is written by hand. The starting point is not the
instructions but a specification of what the module does: the PRMs, the
module's own documentation, its source, and what RISC OS 5.30 is seen to
do on real hardware. Where the documents disagree with the hardware, the
hardware wins.

The Buffer Manager was the first. Its contract is written down
separately, and the C implements that. This is how a byte goes into a
buffer:

```c
/* 1 if the byte could not be inserted: the buffer is full. */
static int insert_byte(struct record *r, uint8_t byte)
{
    uint32_t size = r->size;
    uint32_t ins = atomic_load_explicit(&r->ins, memory_order_relaxed);
    uint32_t rem = atomic_load_explicit(&r->rem, memory_order_acquire);
    uint32_t next = ins + 1 == size ? 0 : ins + 1;
    if (next == rem) {
        if (atomic_load(&r->flags) & F_INPUT_FULL)
            event(EVENT_INPUT_FULL, r->handle, byte, 0);
        return 1;
    }
    *(uint8_t *)ros_ptr(r->start + ins) = byte;
    atomic_store_explicit(&r->ins, next, memory_order_release);
    check_filling(r, ring((int64_t)rem - 1, next, size));
    wake(r);
    return 0;
}
```

There are no registers here and no flags. Where RISC OS disabled
interrupts, this uses C11 atomics. The layout clients can see (the ring,
the indices, the one-unit gap) is kept exactly, because clients read it
directly.

### Accuracy

Tier 2 is the least accurate of the four, and for a reason that cannot
be tested away: it is only as good as the specification. The native
Window Manager was written to a 13-chapter specification taken from the
source, and checked against 5.30 on the farm case by case. Even so,
chapter 02 said a child task that had printed nothing would not have its
mode set again; the farm showed that it does. Every such difference has
to be found by testing, because nothing about the method rules it out.

This is also the tier used where the original source may not be read at
all. BOX's StrongHelp is a clean-room rewrite in C from the file format
and the observed behaviour of StrongHelp 2.90, with no source consulted.

### Speed

Tier 2 is where the speed is. Measured on the Apple silicon box, median
µs per operation, translated (tier 1) against native (tier 2) Window
Manager:

| Operation | Tier 1 | Tier 2 |
| --- | --- | --- |
| Wimp_Poll | 3.0 | 1.3 |
| Message | 3.3 | 1.8 |
| Redraw | 9,517 | 6,308 |
| Task switch (two) | 29.4 | 8.0 |
| Typing | 211 | 114 |

The native Wimp is 17,821 lines of C, about a sixth of the translation.

### Readability

Tier 2 reads as C written by someone who knows the job, because that is
what it is. It can use Linux and libc directly, and it can be changed.

---

## 5. Tier 3: rewritten against the translation

Tier 3 is also written by hand, but it is not written from a
specification. It is written from the translated code, a function at a
time, and the translation stays in place while it is done.

The method is worth setting out, because the testing is what makes it
work:

1. The module is translated at tier 1, and runs.
2. One function is chosen and written again in C by hand, reading the
   tier 0 and tier 1 forms, and the original ObjAsm, to see what it does.
3. The build is arranged so that the hand function *replaces* the
   translated one in the same program. The result is a **twin**: the
   same module, with some functions translated and some hand-written,
   which can be built either way from the same sources.
4. Both builds are run against the same tests, and every value they
   print must be identical.

The oracle is therefore the original code's own behaviour, not a
document. A function that produces different output is wrong, and the
test says so immediately. Nothing rests on anyone's reading of a manual.

### The worked example: BBC BASIC

BASIC is the one component that has been taken all the way through. RISC
OS 5.31's BASICVFP is 23,806 lines of ARM assembler, which ROSASM
compiles into 51,826 lines of tier 1 C. That translation ran correctly
but slowly, for the reasons in section 3: an interpreter is a dispatch
loop, and a dispatch loop is exactly what tier 1 code is worst at.

It has since been rewritten by hand, function by function, against that
translation. The hand-written interpreter is now what the ROM contains
(`BASIC=tier3`, the default), and the machine translation it replaced
has been deleted.

**Accuracy.** The gates are run against real RISC OS 5.30 on a farm of
reference machines: a seven-program suite covering number formatting,
the language, errors, files and `SYS`, floating-point exceptions, 1,600
elementary-function values and workloads — **7 of 7**, the spools
byte-identical — and the 163-program Rosetta Code corpus, **163 of 163**,
byte for byte.

**Speed.** Against the translation it replaced, measured in the box over
nine programs each scaled to take tens of centiseconds, every printed
value identical under both builds:

| Program | Translated (cs) | Rewritten (cs) | |
| --- | ---: | ---: | --- |
| recursive `FNfib(25)`, x20 | 89 | 61 | 1.46x |
| iterative fibonacci, x25 | 52 | 30 | 1.73x |
| nested loops 400 x 400, x50 | 99 | 56 | 1.77x |
| string operations, x50 | 40 | 18 | 2.22x |
| `SQR` and `SIN`, 50000 ops, x50 | 59 | 36 | 1.64x |
| **geometric mean** | | | **1.71x** |

So about one and three quarter times faster, and between 1.46 and 2.22
depending on the work. That is a worthwhile gain and not a
transformation: the interpreter is quicker, but it is still an
interpreter. Compiling is still the way to go fast, and BOX's `*RosBas`
compiler runs the same Mandelbrot kernel some fifteen to twenty times
faster again.

### What tier 3 costs

It is slow work and it needs the translation to exist first. Every
function has to be understood well enough to write again, and the
twin — the arrangement that lets one build use the hand function and
another the translated one — has to be maintained while the work is in
progress. It is worth doing for code that is both heavily used and
badly served by translation. For most modules it is not worth doing at
all.

---

## 6. Choosing a tier

Each module is either translated (tiers 0 and 1) or rewritten (tiers 2
and 3). As a rule:

* The more a module is a service with a documented interface (SWIs,
  vectors, service calls), and the more of its work Linux already does,
  the more likely it is to be rewritten at **tier 2**. Buffers,
  FileSwitch, MessageTrans, the Internet module and the Window Manager
  have all gone this way.
* Where the behaviour *is* the specification — written down nowhere but
  the code, as with a renderer's exact pixels or BASIC's number
  formatting — translation comes first, and **tier 3** is the way to
  make it fast afterwards without risking the behaviour.
* Everything else stays at **tier 1**, which is correct and runs today.

The tiers are not alternatives so much as steps. A translated module
runs at once, at tier 1. Its tier 0 form then serves as the executable
specification against which a rewrite can be tested, whenever the
rewrite is worth doing.
