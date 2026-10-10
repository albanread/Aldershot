# Emulation in BOX

*8 October 2026*

## Introduction

Is BOX an emulator? No — but it contains one.

BOX is a RISC OS application environment for 64-bit computers, and
RISC OS's components in it are native code. The kernel, the Window
Manager, the filing systems, the Font Manager, BASIC and the rest have
been translated or rewritten in C and compiled for the machine they run
on (see *Tiers of Translation*). No processor is emulated to run the
operating system.

For programs that cannot be converted — chiefly ARM binaries whose
source is lost or was never published — BOX also contains an emulator:
the **ARM container**. It emulates an ARM processor in user mode, and
nothing else. When an emulated program calls RISC OS, BOX's native code
answers.

This article explains the difference, what it buys, and how far the
container has got.

---

## 1. What a full emulator does

An emulator such as RPCEmu, Arculator or QEMU pretends to be a whole
machine: an ARM processor, its memory, and its hardware. RISC OS runs on
it exactly as it would on the real thing, ROM image and all. Because the
processor is emulated, anything runs: RISC OS itself, applications in C
or assembler, programs whose source has been lost, and games that poke
the hardware.

That is an emulator's great strength, and it needs nothing from the
program's author.

The cost is speed, because *everything* is emulated — the operating
system's own code most of all. A desktop application spends much of its
life inside RISC OS: redrawing windows, painting text, plotting sprites,
reading files. In a full emulator every one of those instructions is
emulated too.

---

## 2. What BOX does instead

In BOX the operating system is native code, so none of that work is
emulated. When a program calls a SWI it makes a function call, and the
code that answers it is compiled C running at the host's full speed.

Most programs run natively as well:

* **BBC BASIC programs**, run by BOX's interpreter — itself a hand
  translation of RISC OS's own — or compiled by `*RosBas`.
* **C programs**, recompiled for BOX and linked against the ROM's C
  library.

The applications in BOX's ROM show the three native routes. Edit, Draw
and Paint are C, and were recompiled. StrongED is ARM assembler, so it
was rewritten in C from a specification of what it does, keeping its own
data files. ChangeFSI is a BASIC program that loads small ARM routines
at run time; those routines were rewritten in C as a module, and the
BASIC was left alone.

### What that is worth

The Mandelbrot kernel from the `MandelA64` example computes 320 × 256
points, at most 64 iterations each, in BASIC's floating point:

| How it is run | Time |
| --- | --- |
| RISC OS 5.30's BASIC on an emulated Raspberry Pi 4 (QEMU, same Mac) | 2.11 – 2.18 s |
| BOX's BASIC, interpreted | 0.86 s |

That row needs care, and it is worth stating plainly: the first line is
the real BASIC, but running under QEMU's *software* emulation of an ARM
processor, which is itself slow. It is not a Raspberry Pi 4. The
comparison shows that BOX's interpreter beats the original when the
original is emulated. It says nothing in BOX's favour against real
hardware, where a Pi 4 runs RISC OS natively and quickly.

---

## 3. The rule: emulate the processor, never the OS

The ARM container emulates the ARM processor in user mode, and stops
there. It has:

* **no emulated memory system** — the program's memory is BOX's own, at
  the same addresses it would have on RISC OS;
* **no emulated hardware** — no video chip, interrupt controller or
  timers;
* **no second RISC OS** — no ROM image, no emulated kernel, no second
  desktop.

When the program makes a SWI the container stops, BOX's native code
answers it on the same thread, exactly as it answers a native program's
SWI, and the container carries on.

So in an emulated application the emulated part is the smallest it could
be: the program's own instructions, its logic and its loops. Everything
it asks of the system — the Window Manager, the Font Manager, the VDU
drivers, Draw, SpriteExtend, ColourTrans, FileSwitch — is native.

Two kinds of work that are emulated elsewhere are native here as well:

* **Floating point.** ARM programs of RISC OS's era use the old FPA
  instructions, which a full emulator hands to RISC OS's FPEmulator —
  itself emulated ARM code. The container sends each FPA instruction
  straight to BOX's native floating point. No FPEmulator is needed
  anywhere in BOX.
* **The C library.** An ARM C program calls RISC OS's SharedCLibrary.
  BOX's native library cannot take ARM callers directly — the calling
  conventions differ — so the container runs RISC OS 5.30's own library,
  relocated, for those programs.

---

## 4. How it works

### The engine

The engine is **dynarmic**, a just-in-time translator for ARM code. It
turns each block of the program's ARM instructions into the host's own
code the first time that block runs, and keeps the result. It has two
back ends, so the same container runs on both boxes: AArch64 on Apple
silicon and x86-64 on Intel. dynarmic is under the 0BSD licence, so it
brings no copyleft code into BOX.

### Memory without marshalling

In BOX every RISC OS address is a host address (see *BOX Design*). The
container uses that directly: the emulated program reads and writes
BOX's memory at the addresses it would use on RISC OS. Its application
slot is at `&8000`, the RMA and dynamic areas are where RISC OS puts
them, and a block passed to a SWI is the very memory BOX's native code
reads. Nothing is copied across the boundary in either direction.

### Privileged code

User mode is all the processor emulation covers. The privileged parts of
RISC OS — processor modes, and the handlers RISC OS calls on an error,
an escape or an event — are provided natively by the container's bridge,
working over the emulated program's registers. An ARM module's entry
points are run by the container when native code calls them.

---

## 5. ARM modules, and shadows

A difficulty arises that a full emulator never meets. RISC OS allows one
module of any given title, and `RMLoad` kills the old one. An ARM
application that loads its own copy of a module BOX already has natively
would, under that rule, push every caller in the machine into emulated
code — and into the wrong behaviour, for modules BOX does differently.

BOX's rule is therefore:

> Native code uses an ARM module only when there is no native one. ARM
> code may shadow a native module.

An ARM module whose title matches a native one does not replace it. It
becomes that title's **ARM shadow**, and the caller decides which is
reached: an ARM task gets the shadow, native code gets the native
module. SWIs, `*` commands and module lookups all follow the caller.
SharedCLibrary has always worked this way; this makes it general.

Some modules are never shadowed at all — the kernel, FileSwitch, the
Window Manager and the drivers. Where you want the ARM one preferred,
`*ARMPrefer <title> on` says so.

---

## 6. An emulated program is an ordinary task

An ARM application in BOX is a RISC OS task like any other:

* it starts from its own, unedited `!Run` file;
* it has its own application slot, and the Window Manager switches it in
  and out with the rest;
* its windows sit on the desktop beside native applications', and it
  takes part in the usual Wimp messages — a file dragged in from the
  Filer opens in it, as it would on RISC OS;
* if it crashes, RISC OS's own "Application may have gone wrong" box
  appears, Quit removes it, and the desktop and the other tasks carry
  on. A deliberate crash drill — an ARM task that reads from nowhere in
  the middle of a redraw — ends that way ten times out of ten.

ARM modules load with `*RMLoad`, utilities run as on RISC OS, and an ARM
program can run in a task window. A BBC BASIC program that assembles ARM
code is recognised automatically: BASIC's assembler normally assembles
for the host processor, but a program that is plainly writing ARM is
switched to ARM for its own routines, which then run in the container.
`*BasicAsmCPU` overrides the choice if you want to be explicit.

---

## 7. What it runs today

This is the part to be honest about: the container is new, and most ARM
software has not been tried in it.

**It works.** OvationPro 2.78 — a commercial desktop publishing
application available only as an ARM binary — starts from its stock
`!Run` with its own ARM modules, opens documents, takes typing, saves
its files, and loads ArtWorks pictures through the ArtWorks renderer,
itself ARM code. It does this on both the Intel and Apple silicon boxes.
Command-line ARM programs run in a task window with their output
matching real RISC OS 5.30's, and BASIC programs with ARM routines run,
such as the AWViewer ArtWorks viewer.

**The harder test is the acceptance set**, which takes the binary-only
ARM applications from a RISC OS test disc and starts each one in a fresh
desktop box after its own `!Boot`. Of seventeen, **eight start today**:
7bupstats, Allocate, InterGif, Patch, PrivatEye, Snapper, SyncDiscs and
T1ToFont.

The rest fail for reasons that are being worked through one at a time,
and the reasons are ordinary ones: a module that calls `OS_Exit` from
module code, squeezed modules that the loader now unsqueezes, an
undefined instruction, a missing `Alias`, applications wanting
`!Unicode` and Iconv, and others wanting modules BOX has not yet got.
Each is a fix in the bridge or a marked patch, not an exception.

RISC OS 5.30's DrawFile and Toolbox modules — seventeen of them — are
currently run emulated from `System:Modules` to support these
applications. That is a stopgap: they have published source, and are to
be ported natively as ColourPicker was.

### Measuring it

`*ARMStats` reports how the time divides between the engine, native code
and floating point, with samples of where the program's own code is
spending it. What the first measurements showed was mostly negative and
worth knowing: extra processors make no difference, pinning the
container to its own core is *worse*, and on Intel the check that lets
the engine be interrupted costs about 38 per cent.

What has **not** been measured is the thing people will most want to
know — how much faster a given application is here than in a full
emulator. It depends entirely on how much of its time that application
spends inside RISC OS, and we have not yet published a figure. Saying
so is better than guessing.

---

## 8. What it does not do

* **26-bit code.** Software that runs only on the 26-bit RISC OS, before
  RISC OS 5, is not supported.
* **Thumb.** RISC OS programs are ARM code, and only ARM code is
  supported.
* **Hardware.** A program that pokes the hardware directly has nothing
  to poke: the access faults and the program gets an error.
* **Everything.** Where something does not run, a full emulator is still
  the way to run it.

---

## 9. Which to use

| | Full emulator | BOX |
| --- | --- | --- |
| RISC OS's components | emulated | native |
| ARM binaries | emulated | emulated, in the ARM container |
| BASIC | emulated | native |
| C programs | emulated | native, after a recompile |
| Hardware | emulated, often to the chip | Linux's drivers |
| Networking | whatever the emulator offers | Linux's, with TLS 1.3, SSH and SMB3 |

If you want to run old software exactly as it was, games that poke the
hardware, or anything the container does not yet handle, use an
emulator. If you want a RISC OS application environment that runs
quickly on modern hardware, with your own programs in BASIC or C and an
emulator to hand for the binaries you cannot do without, BOX is the
better home.

---

## 10. Native first

The container is a bridge, not a destination. The rule is to run as much
as possible natively and as little as possible emulated, and each piece
of support the container needs is done natively unless there is no other
way.

Where the source exists, translation or a recompile remains the better
route: the program then runs at full native speed with nothing emulated
at all. The container is for the software for which that will never
happen — and there is a good deal of it.
