# Tiers of Translation

*3 October 2026*

## Introduction

Most of RISC OS is written in ARM assembler. BOX runs no ARM code, so
every part of RISC OS that BOX uses has to become C in one way or
another. There is more than one way of doing this, and the results differ
a great deal in accuracy, speed and how easy the code is to read.

This article describes four levels, or *tiers*, of translated code:

| Tier | Name | How it is made | Accuracy | Speed | Readability |
| --- | --- | --- | --- | --- | --- |
| 0 | Unlifted | `rosasm --emit c --no-lift` | exact | poor | very poor |
| 1 | Lifted | `rosasm --emit c` | exact, if the lifter is right | fair | fair |
| 2 | Rewritten | by hand, from a specification | as good as the specification | good | good |
| 3 | Readable | tier 2, tidied for people | as tier 2 | as tier 2 | best |

Tiers 0 and 1 are produced by the compiler. Tiers 2 and 3 are written by
people (or by an LLM working under test), using a lower tier as the
reference.

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

## 1. Tier 0: unlifted code

Tier 0 is what you get if you take a JIT compiler and run it ahead of
time. Each ARM instruction becomes one C statement, working on a block of
registers held in memory. Flags are fields in the same block. Branches
are labels and `goto`s. The guest stack is ordinary arena memory, reached
through R13.

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
decoder or the assembler front end has got an instruction wrong. Two such bugs were found in rosasm's FPA
handling: every FPA compare was counted as two words, so every label after
one was four bytes out (SharedCLibrary was among the victims), and
post-indexed transfers such as `LFM f4, 4, [sp], #48` lost their offset
and writeback. Neither was a fault in the RISC OS code being translated.

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

## 2. Tier 1: lifted code

Lifting is the compiler's next step. It is still automatic, and the code
is still partly an emulation of the machine, but it now looks like C.

* Registers become C local variables (`r0`, `r1` and so on), and only
  go back to the register block where a call, a SWI or a return needs
  them.
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
constants had been swapped: a 12° italic came out as 0.37°. The fault had
been there all along; one statement per instruction had hidden it.

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

## 3. Tier 2: rewritten from a specification

Tier 2 code is written by hand. The starting point is not the
instructions but a specification of what the module does: the PRMs, the
module's own documentation, its source, and what RISC OS 5.30 is seen to
do on real hardware. Where the documents disagree with the hardware, the
hardware wins.

The Buffer Manager was the first. Its contract is written down
separately (`rosgd/modules/buffers/README.md`), and the C implements
that. This is how a byte goes into a buffer:

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

Tier 2 is less accurate than tiers 0 and 1. It is only as good as the
specification, and specifications have gaps. The native Window Manager
was written to a 13-chapter specification taken from the source, and
checked against 5.30 on the farm case by case. Even so, chapter 02 said a
child task that had printed nothing would not have its mode set again;
the farm showed that it does. One StrongED fault is still open that
appears only under the native Wimp. Every such difference has to be
found by testing, because nothing about the method rules it out.

### Speed

Tier 2 is where the speed is. Measured on the Apple Silicon box, median
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

## 4. Tier 3: readable code

Tier 3 is tier 2 code that has been gone over deliberately to make it
easier to read: proper names in place of register-shaped ones, long
routines split, hand-rolled idioms replaced by library calls, and
comments that explain why rather than what. Behaviour does not change, so
the speed and accuracy are those of tier 2.

The rule for this work is the same as for the other tiers: a rewrite is
accepted only if it gives the same results as the code it replaces,
under the same tests. Much of
the routine work here can be done by an LLM, since the harness, not the
reviewer, decides whether a change is accepted.

A component reaches tier 3 when a person has reviewed it and taken it
over. From then on it is ordinary source, maintained by hand. No module
has yet been through a separate tier 3 pass.

---

## 5. Choosing a tier

Each module is either translated (tiers 0 and 1) or reimplemented
(tiers 2 and 3). As a rule:

* The more a module is a service with a documented interface (SWIs,
  vectors, service calls), and the more of its work Linux already does,
  the more likely it is to be reimplemented. Buffers, FileSwitch,
  MessageTrans, the Internet module and now the Window Manager have all
  gone this way.
* Where the behaviour *is* the specification, written down nowhere but
  the code (a renderer's exact pixels, BASIC's number formatting), it
  is better translated first.

The tiers are not alternatives so much as steps. A translated module
runs at once, at tier 1. Its tier 0 form then serves as the executable
specification against which a tier 2 rewrite can be tested, whenever the
rewrite is worth doing.
