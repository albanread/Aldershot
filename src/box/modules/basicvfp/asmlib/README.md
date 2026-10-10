# BASIC inline assembler: x86-64 and AArch64

BBC BASIC's inline assembler (`[ ... ]`) assembles ARM code on RISC OS. In
the BOX it can also assemble x86-64 (`asmx64.c`) and AArch64 (`asma64.c`).
The ROM's own ARM assembler, from the ROOL BASIC sources, still runs the block
for every CPU. The encoders only supply the bytes for each instruction.
`../patch-basicasm.py` hooks the ARM assembler, and `basicasm.c` is the hook.

## Use

    *BasicAsmCPU X64    x86-64 (the default in the box and on an Intel Mac)
    *BasicAsmCPU A64    AArch64 (the default where the build's TARGET is AArch64)
    *BasicAsmCPU ARM    the ROM's ARM assembler
    *BasicAsmCPU        back to the default

The choice belongs to the task. A new task starts with the default. The
default comes from `BASICASM_DEFAULT_CPU`, which the Makefile sets. Both
encoders are built unless `BASICASM=x64`, `a64` or `none` is given.
`*BASIC105` is ARM only.

Labels are BASIC variables, `P%`, `O%`, `L%`, `OPT` and the passes work as
for ARM code, and errors use BASIC's own numbers. An operand is valued by
BASIC's own evaluator, so variables, `FN` and the BASIC operators work.

## Running assembled code

`]` registers the code and `CALL` and `USR` enter it.

- x86-64 is entered as a SysV function. `%rdi` points to the register block
  (R0 to R15 and the PSR as 32-bit words). `CALL` loads `A%` to `H%` into R0
  to R7, and `USR` returns R0. The code preserves `%rbx`, `%rbp`, `%r12` to
  `%r15` and `%gs`, and leaves with `ret`. SWIs are called through the gate at
  `&FEEFF000`.
- AArch64 is entered as an AAPCS64 function, with `x0` the register block and
  `x1` the SWI entry. It runs unprivileged in a box on AArch64.

The hosted build assembles but does not run assembled code. Code for a CPU
other than the machine's is not registered, and `CALL` to it gives "not
compiled code". `Examples.MandelX64` and `Examples.MandelA64` are working
examples.

## Differences from RISC OS 5.30

RISC OS has no `*BasicAsmCPU`. A program that assembles ARM code needs
`*BasicAsmCPU ARM` first. `SYS "XOS_CLI","BasicAsmCPU ARM"` works on both.
Immediate and displacement forms follow LLVM MC, so a value that MC would
truncate silently is a BASIC error here.

## Tests

- `make asmtest-all` here checks the encoders against the corpora named in
  `ORACLES` and against `goldens/`, all taken from LLVM MC.
- `tests/basic/asm/compare.py`, run by `make test`, runs programs written for
  ARM, x86-64 and AArch64. The ARM output was recorded on RISC OS 5.30.
- `boot/selftest_basicvfp.c` assembles for both CPUs.
- `tests/a64x32/mandel.py` runs the AArch64 example.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
