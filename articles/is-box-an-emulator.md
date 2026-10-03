# Is BOX an Emulator?

*3 October 2026*

## Introduction

No. BOX is a port of RISC OS to C, running on Linux. It does not emulate
an ARM processor, and it cannot run ARM code. Everything it runs has
been translated or compiled to native code for the machine it is on.

That makes BOX quick, and it lets RISC OS use Linux's drivers and
networking (see *Is BOX Linux?*). It also means BOX cannot run every
RISC OS program as it stands. This article explains the difference
between BOX and an emulator, what runs on BOX unchanged, and what has
to be converted first.

---

## 1. What an emulator does

An emulator such as RPCEmu, Arculator or QEMU pretends to be a whole
machine: an ARM processor, its memory, and its hardware. RISC OS runs
on it exactly as it would on the real thing, ROM image and all. Since
the processor is emulated, any ARM code runs: RISC OS itself,
applications written in C or assembler, programs whose source has been
lost, and games that poke the hardware.

That is an emulator's great strength. It runs anything the real
machine ran, and needs nothing from the program's author.

The cost is speed. Every ARM instruction has to be decoded and
translated on the fly. RISC OS 5.30's BASIC, running a Mandelbrot
benchmark on an emulated Raspberry Pi 4 under QEMU, takes 2.1 seconds
on an M4 Max. BOX's BASIC takes 0.86 seconds on the same Mac (see *BBC
BASIC in Translation*).

---

## 2. What BOX does instead

BOX has no ARM processor, real or emulated. RISC OS itself has been
converted to C, either by translating its assembler (ROSASM) or by
rewriting modules from a specification, and the result is compiled as
native code for the host. When a program calls a SWI it makes a
function call, and the OS code that answers is native code too.

So BOX can run only code that has been converted in the same way:

* **RISC OS's own modules**, translated or rewritten, in the ROM.
* **BBC BASIC programs**, run by the translated interpreter, or
  compiled by ROSBAS.
* **C programs**, compiled for BOX by clang (`*CC`, or the Mac's
  toolchain) and linked against the ROM's C library.

There is nothing in BOX that could run an ARM instruction it was given.

---

## 3. What runs unchanged

**BBC BASIC programs run unchanged**, as long as they do not use the
assembler. BOX's BASIC is RISC OS 5's own interpreter, translated, and
its output is checked line by line against real RISC OS 5.30. Of the
163 Rosetta Code programs used as a test corpus, all give the same
output under BOX's interpreter as under 5.30.

A program written entirely in BASIC, which includes a good many desktop
applications, needs no conversion at all.

---

## 4. What needs converting

Many RISC OS programs are not pure BASIC. A typical application has
some of:

* a `!Run` file and BASIC, which run as they are;
* **C**, compiled to ARM code;
* **assembler**: a module, a fast routine called from BASIC, or a
  hand-written application.

Any ARM code, of whatever origin, has to be converted before BOX can
run it.

| Program contains | What is needed |
| --- | --- |
| BASIC only | nothing |
| BASIC that assembles ARM code | the assembler parts rewritten, in BASIC, C, or BOX's host assembler |
| C, with source | a recompile for BOX, and any fixes for code that assumed it was on ARM |
| ObjAsm, with source | translation by ROSASM, or a rewrite |
| ARM code with no source | cannot run on BOX |

The applications in BOX's ROM show what this means in practice.
Edit, Draw and Paint are C, and were recompiled. StrongED is written in
ARM assembler, so it was reimplemented in C from a specification of what
it does, keeping its own data files. ChangeFSI is a BASIC program that loads small ARM routines at run
time; those routines were rewritten in C as a module, and the BASIC was
left as it was.

**This is the main drawback of BOX compared with an emulator.** An
emulator runs a program the day you find it. BOX runs it once someone
has converted it, and that needs the source.

---

## 5. Which to use

| | Emulator | BOX |
| --- | --- | --- |
| Runs ARM binaries | yes | no |
| Runs BASIC unchanged | yes | yes, without the assembler |
| Runs C programs | yes | after a recompile |
| Needs the source | no | yes, for anything not in BASIC |
| Speed | the cost of emulation | native |
| Hardware | emulated, often down to the chip | Linux's drivers |
| Networking | whatever the emulator offers | Linux's, with TLS 1.3, SSH and SMB3 |

If you want to run an old program, an old game, or anything whose
source is gone, use an emulator. If you want RISC OS to run fast on
modern hardware, and you can write or convert your programs in BASIC
or C, BOX is the better home.

Since BOX can run any program written for it, it could in principle
run an emulator too. That is code that has not been written yet, and
this article does not discuss it.
