# BOX Tools

*3 October 2026*

## Introduction

RISC OS has always been a machine you can program. BBC BASIC is in the
ROM, an Obey file is a script, and the BASIC assembler lets you drop into
ARM code in the middle of a program. BOX keeps all of that. The BASIC
interpreter is there, translated like the rest of the ROM, and it runs
BASIC programs as it always did.

What is different is underneath. In BOX there is no ARM code at all:
every part of RISC OS is C, compiled for the host processor. So the tools
that make software for BOX are tools that write C, or compile it. This
article describes them:

* **x32 mode**, the C model that RISC OS programs are compiled for
* **`*CC`**, the C compiler in the box
* **ROSASM**, the assembler that turns RISC OS's ObjAsm into C
* **ROSBAS**, the compiler that turns BBC BASIC V into C
* **roscc**, the linker behind all of them.

---

## 1. x32 mode

A RISC OS program expects its pointers to be 32 bits. Structures passed
to SWIs have 4-byte pointers in them, a Wimp message block is laid out
for 4-byte pointers, and programs store addresses in integers. BOX keeps
every byte of RISC OS memory in the bottom 4 GB of the address space (the
*arena*; see *BOX Architecture*), so a 32-bit pointer is enough to reach
all of it.

The host processors, however, are 64-bit. The answer is an x32 mode: the
full 64-bit instruction set, with 32-bit pointers. `int`, `long`,
`size_t` and pointers are all 4 bytes (the ILP32 data model). Code
compiled this way can only ever form an address inside the arena.

There are two forms, one for each processor:

| | x32 (Intel) | A64X32 (Apple silicon) |
| --- | --- | --- |
| Instructions | x86-64 | A64 |
| Pointers, `long`, `size_t` | 4 bytes | 4 bytes |
| How addresses stay below 4 GB | the compiler puts an `addr32` prefix on every memory access not based on the stack | addresses are formed in 32-bit W registers, which wrap at 4 GB; the few 64-bit sums left fault into a guard region from 4 GB to 68 GB |
| Static base | `%gs` | `x18`, reserved in all arena code |
| Compiler | clang `-mx32` | clang with BOX's ILP32 front-end patch |

x32 code never makes a Linux system call. It reaches the operating
system only through the SWI gate, a fixed page in the arena at
`&FEEFF000`, which switches to a native stack above 4 GB and calls the
SWI dispatcher. Linux therefore needs no ILP32 support of its own; this
matters on AArch64, where Linux has none.

### What is compiled x32

* the ROM's SharedCLibrary
* C modules, such as the Toolbox modules and ColourPicker
* C applications: Edit, Draw, Paint, StrongED, PipeDream, NetSurf
* the output of ROSBAS
* what `*CC` builds.

### What is not

The runtime (`/init`) and every translated ObjAsm module are ordinary
64-bit code (LP64), loaded above 4 GB. They never hold a RISC OS address
as a C pointer. Instead they reach the arena through accessors such as
`ros_ld32()`, which, because the arena is mapped at address 0, compile to
a single load. Building them x32 as well would gain nothing and would
put the runtime's own memory inside the arena, where RISC OS programs
could overwrite it.

---

## 2. *CC: the C compiler

On the Apple silicon box `*CC` is clang: the same patched clang that
builds A64X32 code on the Mac, linked with lld into one static program
and carried in the box. You use it from the command line or an Obey
file, with RISC OS file names:

```
*CC c.hello
*hello
```

By default `*CC` makes a **RISC OS application**. Each source is compiled
A64X32 against RISC OS's own C headers, and roscc links the objects
against the ROM's SharedCLibrary. The result is typed `&FF8` and runs in
its own application slot, like any other application; `*WimpSlot`, Wimp
tasks and `*Run` all work as usual.

With `--linux`, `*CC` makes a **Linux program** instead: a static LP64
program linked against musl, typed ELF (`&E1F`). It runs outside RISC OS,
as a child process on a pseudo-terminal, through `*RunBox`. Its output
is drawn in the text window, it reads the RISC OS keyboard, and if it
crashes only the child process ends. This is how you run ordinary Unix
software from the box.

Naming follows RISC OS custom: `c.hello` builds `hello`, beside the `c`
directory. A program that would overwrite one of its own sources is
refused, and a failed compile is an error, so an Obey build stops at the
first failure.

It is quick. Timed in the box, on two virtual CPUs of an M4 Max, a C
"hello" compiles and links to an application in 0.02 to 0.04 seconds.
The objects and images the box makes are byte for byte the same as those
the Mac's clang and roscc make from the same source.

---

## 3. ROSASM: the assembler that writes C

Most of RISC OS is written in ObjAsm, Acorn's ARM assembler. ROSASM is a
compatible assembler written in Rust. It reads the RISC OS 5 sources as
they are, macros, conditional assembly and `GET` files included, and its
output is held to the DDE's own ObjAsm by comparison: listings column by
column, objects byte for byte, and the real ObjAsm run on an emulator as
the reference.

As an assembler it writes AOF or ELF objects. With `--emit c` it
compiles instead: it writes the unit's ROM image, and its code as C. This
is how the whole of BOX's ROM is made, the Window Manager and BASIC
among it. The C comes in two forms, described in *Tiers of Translation*:

* **tier 0** (`--no-lift`): one C statement per instruction over a block
  of registers. Exact, slow and hard to read; the reference.
* **tier 1** (the default): lifted, with registers in local variables,
  flags turned into comparisons, and real loops. Tested against tier 0.

The author's comments are carried into the C beside the code they
describe, and constants and workspace fields keep their names.

ROSASM runs where the toolchain is, on the Mac, because its output is
built into the ROM. The `*RosAsm` command exists in the hosted runtime,
where it runs ROSASM on RISC OS file names. In the box itself it refuses,
with error `&C0125`.

---

## 4. ROSBAS: BBC BASIC compiled

ROSBAS compiles BBC BASIC V programs to C. The language it accepts is
RISC OS 5.30's BASICVFP, quirks and all, and it is pinned down by a
written specification in fifteen chapters. Each point in the
specification is backed by a conformance test: a small program and the
output real RISC OS 5.30 gave for it.

* **Loading** tokenises text exactly as BASIC's `TEXTLOAD` does, giving
  the same bytes and line numbers as the interpreter on 610 conformance
  tests and 163 Rosetta Code programs.
* **The front end** parses each statement as the interpreter does, and
  makes the interpreter's run-time scans (for `ENDPROC`, `NEXT`, the end
  of a `CASE`) once, at compile time.
* **The back end** writes C, which calls a runtime library for
  arithmetic, strings, `PRINT`, errors, `LOCAL`, files and the operating
  system.

In the box, `*RosBas` does the whole job:

```
*RosBas bas.game
*game
```

ROSBAS makes the C, clang compiles it, and roscc links it with the
ROSBAS runtime against the ROM's SharedCLibrary. The result is an
ordinary RISC OS application, typed `&FF8`. A five-line program builds
in 0.07 seconds, and the `Primes` example in 0.08 to 0.10 seconds.

### How accurate it is

On the farm's real RISC OS 5.30, the conformance suite passes 640 of 649
test runs. On the host, 162 of the 163 Rosetta programs give the same
output as the interpreter. Still to do are `CALL` and `USR`, which call
machine code and so need thought in a system with none, and one gated
quirk.

---

## 5. roscc: the linker

roscc is the back end common to all of these. It takes ELF32 objects with
32-bit pointers and writes RISC OS images:

* **`link --clib`** makes an application a client of the ROM's
  SharedCLibrary. It adds the library's stubs and a start file whose
  `_start` registers with the library, as a RISC OS C program always has,
  and the library then calls `main`.
* **`link --module`** makes a relocatable module, with its header and
  veneers. `tools/cmhg.py` reads the DDE's `.cmhg` files to make them.
* **`link --clib-image`** makes the ROM's C library itself: one
  read-only image, with each client's library statics laid out in that
  client's own memory.

roscc also links for the original ARM target, so the same objects and the
same module descriptions can make a Raspberry Pi image.

---

## 6. Building in the box

These tools are driven from Obey files, as RISC OS builds always have
been:

```
*Obey Build
  CC      c.main c.util -o main
  RosBas  bas.game
  game
```

The tools report errors in the usual way (`&C0120` upwards), so a failed
step stops the build. Everything a build makes is an ordinary RISC OS
file, and runs by name or by a double-click in the Filer.
