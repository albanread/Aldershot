# BBC BASIC V, translated to C

These files are RISC OS's BBC BASIC V interpreter (BASICVFP), translated
by hand from ARM assembler into C. They are the BASIC in the BOX's ROM.

The original is RISC OS Open's BBC BASIC, `Sources/Programmer/BASIC`, as
in RISC OS 5.31: about 24,000 lines of 32-bit ARM assembler. These files
are a translation of it, so they keep its licence, the Apache License 2.0.
Each file names the sources it translates and carries their copyright
lines. See [LICENSE](LICENSE).

## What is here

34 C files and their headers, 258 functions in all:

- every statement of the language: assignment, control flow, input and
  output, graphics and the system statements;
- the expression evaluator and all its operators;
- the tokeniser, and `LIST`;
- the variable lookup and creation;
- `FN` and `PROC` calls, parameters and return;
- the error system;
- the `>` prompt and its commands, and the module's start-up.

The translation keeps the original's behaviour exactly, including its
quirks. Each kept quirk is noted in the code, with the line in the
original assembler it comes from (for example `Stmt.s:640-644`).

## What is not here

These files are not a complete program on their own. In the BOX they are
compiled together with:

- parts of the original that are still compiled from its ARM source by
  ROSASM: the ROM image and its tables, the module's registration table,
  the two-statement trunk (TWOSTMT) and the inline assembler;
- the BOX runtime, which provides the register block, memory access and
  SWIs (`rosgd/cpu.h` and others).

Neither is published here yet.

## Testing

The translation was checked against real RISC OS 5.30:

- **A suite of seven programs** covering number formatting, the
  language, errors, files and `SYS`, floating-point exceptions, 1,600
  values of the elementary functions, and some workloads. All seven
  give output byte for byte the same as RISC OS 5.30's.
- **The Rosetta corpus**, 163 BBC BASIC programs from Rosetta Code. 162
  give the same output as RISC OS 5.30. The one difference is
  Hofstadte3: RISC OS 5.30 did not finish it within the recording time,
  so its recorded output is empty.

The translated C runs about 1.6 times as fast as the machine translation
it replaced, measured in the BOX over nine programs.
