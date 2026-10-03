# Porting a C Application to BOX

*3 October 2026*

## Introduction

*Is BOX an Emulator?* says that a RISC OS program written in C must be
converted before BOX can run it. This article describes what that
conversion is in practice, using the applications already ported: Edit,
Draw and Paint, PipeDream, SparkFS and ChangeFSI.

In short: you recompile the C with clang, link it with roscc against
the ROM's C library, replace any assembler with C, and fix the handful
of places where the code relied on the ARM processor or on the Norcroft
compiler. Most of the C compiles unchanged.

---

## 1. What you need

* **The source.** BOX cannot run ARM binaries, so a program without its
  source cannot be ported this way.
* **The toolchain.** clang, for x32 on Intel or A64X32 on Apple silicon
  (see *BOX Tools*); roscc, to link; and RISC OS's own C headers, staged
  by the build (`make capps`).
* **The libraries the program uses.** The ROM's SharedCLibrary is
  there already. RISC_OSLib (`rlib`) is built for BOX from RISC OS's
  sources, with its few ObjAsm files rewritten in C.

---

## 2. Compiling

The aim is to compile the program as Norcroft compiled it on RISC OS,
so that it lays out memory and behaves the same. The compiler flags
BOX uses for every C application do most of this:

| Flag | Why |
| --- | --- |
| `-funsigned-char` | `char` is unsigned on ARM, signed on x86-64 |
| `-fpack-struct=4`, `-mms-bitfields` | structure layout as Norcroft's, so blocks passed to SWIs and saved in files match |
| `-mlong-double-64` | `long double` is a `double`, as in Norcroft |
| `-fno-strict-aliasing` | old RISC OS C freely reads one type through a pointer to another |
| `-ffp-contract=off` | no fused multiply-add, so floating point results match |
| `-D__riscos -D__APCS_32` | the macros RISC OS code tests for |

Pointers are 32 bits (x32 or A64X32), so structures that hold pointers
keep their RISC OS sizes, and an `int` can still hold an address, as
much old RISC OS code assumes.

Old code is often 1990s C, with implicit `int` and K&R declarations.
clang accepts it with the relevant warnings turned off. It is not
worth modernising code that works.

---

## 3. Linking

roscc links the objects into a RISC OS image:

* `roscc link --clib` makes an application (`&FF8`), a client of the
  ROM's SharedCLibrary, loaded at `&8000`;
* `roscc link --module --clib` makes a relocatable module (`&FFA`), its
  header and veneers made from the program's own `.cmhg` file by
  `cmhg.py`.

Libraries are linked as archives, so only the members a program needs
are taken, as with a Unix linker.

---

## 4. Replacing the assembler

Most C applications of any size carry a little ObjAsm: SWI veneers, a
`wimp_poll` wrapper, a table of data, or a fast inner loop. BOX cannot
run it, so each file is rewritten in C. The rule is to do exactly what
the assembler did, no more:

* **SWI veneers** become calls through the SWI gate, keeping the
  original's handling of the V flag and of registers returned.
* **Data tables** (`DCD`, `DCB`, `ALIGN`) become C arrays with the same
  words, laid out as the assembler laid them out.
* **Calls that switched static data**, such as SparkFS's veneers into
  its codecs, become ordinary C calls, since in BOX both sides are C of
  the same ABI.

SparkFS shows the scale of this: one ObjAsm file per component, each
replaced by a short C file. SparkFSApp's `s/swi` and `s/poll` became
`appswi.c` and `apppoll.c`, as RISC_OSLib's did. ChangeFSI, a BASIC
program that assembles ARM routines as it runs, kept its BASIC, and its
routines became a C module (`CFSI`) whose entry points BASIC calls as
before.

---

## 5. What breaks

The C that compiled correctly with Norcroft for ARM sometimes depended
on things C does not promise. These are the faults found so far, and
each was a small, local fix:

* **Shifts by 32 or more.** ARM's shifter gives 0 for a 32-bit shift;
  x86-64 and A64 take the count modulo 32, and clang may drop the code
  altogether. SparkFS's tar and cpio codecs split a time into five bytes
  this way, and every date in an archive came out as 1900.
* **Writing to string literals.** Norcroft kept each literal separate,
  and RISC OS let a program write to them. clang merges identical
  literals and puts them in read-only memory. SparkFSApp filled ten
  identical blank strings with archive names, and every entry in its New
  archive box became "Zip".
* **Reading past the end of a buffer.** Code that happened to work
  because of what lay on the ARM stack may not work with a different
  stack layout. One such fault in SparkFS's cpio codec turned out to be
  a real bug on RISC OS 5.30 as well.
* **Header lineages.** PipeDream compiles against two generations of
  Wimp headers, RISC_OSLib's and the Toolbox library's. The port
  reconstructed both from the real files rather than merging them, and
  changed only three things in the original source. Its notes record the
  lesson of a first, failed attempt: be minimal.

Patches go in the port's own directory and are applied to a staged copy
of the source, never to the original, each with a note saying what was
wrong and why the fix is right.

---

## 6. The application around the program

A RISC OS application is more than its `!RunImage`. The `!Run` file
usually needs a few changes too:

* **`*WimpSlot`.** 64-bit code is larger than ARM code (see *The Native
  Wimp*). SparkFSApp's `!RunImage` is 141 KB and the C library's root
  stack 256 KB, so its slot went from 128 KB to 512 KB. Where `!Run`
  sets no maximum, BOX gives the task as much as it needs.
* **Lines that run ARM code.** A `!Run` that loads an ARM module or runs
  an ARM utility must use the BOX equivalent, or lose the line. BOX
  provides stand-ins where it can: a stub FPEmulator, so `RMEnsure
  FPEmulator` lines pass, and `*IfThere` in BootCmds.
* **`RMEnsure` lines** that load a module from the application, where
  the module is now in BOX's ROM, start the ROM's copy instead.

Each change carries a comment in the file saying why.

---

## 7. Testing

A port is tested by using it, and by comparing it with RISC OS 5.30:

* **The desktop probes** drive the application through the Filer and
  its own windows, as a user would: open a file, edit it, save it.
* **Output is compared with 5.30's.** ChangeFSI's 234 conversions were
  compared with 5.30's, pixel for pixel where 5.30 itself is right.
  SparkFS's archives were compared for listings, types and bytes.
* **Both boxes.** Every port builds and is tested on both the Intel box
  (x32) and the Apple silicon box (A64X32).

---

## 8. The ports so far

| Application | What it took |
| --- | --- |
| Edit, Draw, Paint | recompiled from RISC OS's sources by the common build; now in the ROM |
| PipeDream 4.63 | the original C recompiled, with reconstructed headers and three source changes |
| SparkFS | all of its C unchanged; four patches; one ObjAsm file per component rewritten in C |
| ChangeFSI | its BASIC kept, with two patches; its ARM routines and its JPEG and PNG readers rewritten as a C module |
| StrongED | written in ARM assembler, so reimplemented in C from a specification rather than ported |

In each case most of the work went on the assembler, the `!Run` file
and testing, not on the C.
