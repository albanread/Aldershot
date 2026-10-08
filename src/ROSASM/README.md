# ROSASM

An ObjAsm-compatible assembler for RISC OS 5, written in Rust. It reads
ObjAsm source -- the language the RISC OS 5 sources are written in -- and
does one of two things with it:

- assembles it, to AOF object files for the RISC OS linker, or ELF32
  objects with `--elf`. The code is 32-bit ARM (AArch32).
- compiles it to C, with `--emit c`. This is how the BOX runs RISC OS's
  own assembler sources -- the kernel, the Wimp, BBC BASIC -- on machines
  that are not ARM. See [Compiling to C](#compiling-to-c).

Experimental software, unsupported. MIT licence: see [LICENSE](LICENSE).

## Building

You need Rust (`cargo`) to build it, and clang to run it.

```
cargo build --release
```

rosasm does the ObjAsm language itself -- macros, conditional assembly,
symbols, layout -- and hands each instruction, written out in UAL, to
LLVM's integrated assembler for encoding. So it needs a clang that can
assemble for `arm-none-eabi`; the clang that comes with Xcode, or any
LLVM release, can. It runs `clang` from the path, or the one named by
`--clang path` or the `ROSASM_CLANG` environment variable.

The programs are put in `target/release`: `rosasm` itself, and `aofdump`,
which lists an AOF object.

## Assembling a file

```
rosasm hello.s -o hello.o
```

A small example, `hello.s`:

```
        AREA    |Hello$$Code|, CODE, READONLY

        EXPORT  hello
hello
        ADR     r0, message
        SWI     &2              ; OS_Write0
        MOV     pc, lr

message
        DCB     "Hello, world", 10, 13, 0
        ALIGN

        END
```

`rosasm` reports what it wrote:

```
hello.o: 28 bytes in 1 area, 4 symbols
```

To look inside the object:

```
aofdump hello.o
```

## Options

| Option | Meaning |
|---|---|
| `-o file` | the object file to write |
| `-I dir` | a directory to search for `GET` / `INCLUDE` files; may be given more than once |
| `-PD "name SETA 1"` | define a variable before assembly, as ObjAsm's `-PD` |
| `--elf` | write an ELF32 object instead of AOF |
| `--map file` | write a listing of where each label ended up |
| `--warn-assertions` | report a failed `ASSERT` as a warning, not an error |
| `--allow-unencodable` | do not stop at an instruction that cannot be encoded |
| `--fpa-to-vfp` | assemble FPA floating-point instructions as VFP |
| `--clang path` | the clang to encode with |
| `--keep-temps` | keep the intermediate UAL and object files |

`rosasm` with no arguments prints the full usage.

## Compiling to C

```
rosasm hello.s --emit c --rom-base FC000000 -o hello.c
```

This assembles the unit as usual, then compiles its code to C instead of
writing an object:

```
hello.c: 1 regions, 3 instructions compiled; hello.h beside it
```

Two files are written:

- **hello.c** holds the unit's image -- its bytes, placed at the
  `--rom-base` address -- and its code as C functions. Compiled code reads
  the unit's tables, strings and error blocks at the addresses the source
  gave them, so the base address is required.
- **hello.h** gives the address of every label, as `HELLO_<label>`.

For the example above, the routine `hello` comes out as:

```c
static void hello_hello(struct ros_cpu *s)
{
    uint32_t r0, r14 = R[14];
    r0 = HELLO_message;                         /* ADR r0, message */
    R[0] = r0;                                  /* SWI &2 ; OS_Write0 */
    ros_swi(s, 0x2u);
    r0 = R[0]; r14 = R[14];
    R[15] = r14;                                /* MOV pc, lr */
    return;
}
```

Each instruction keeps its exact ARM meaning, and its source line goes
beside it as a comment. The author's whole-line comments go before the
code they describe.

**How the code is divided.** Code is compiled in *regions*. A region is
an entry point and everything reachable from it without a call. An entry
point is an address something takes: a module header offset, a relocated
word, an `ADR` to code, an `EXPORT`, or the target of a `BL`. A branch to
another region is a guaranteed tail call (`ROS_TAIL_CALL`, clang's
`musttail`), so a chain of them, such as an interpreter's statement loop,
does not grow the C stack. The file ends with a table of the entry points,
which `<unit>_register()` hands to the runtime.

**Two tiers.** By default rosasm *lifts* the code (tier 1). Each block
becomes the expressions it computes, the registers become C locals, the
flags are worked out only where something reads them, and branches become
`if`, loops and `switch`. With `--no-lift` (tier 0), each instruction is
compiled on its own as a statement over the register block, with labels
and `goto`s. Tier 0 is the reference tier 1 is tested against.

**Nothing is guessed.** Anything that cannot be compiled exactly -- a
computed branch it cannot follow, say -- is reported as an error, and no C
is written.

### Options for `--emit c`

| Option | Meaning |
|---|---|
| `--rom-base hex` | the address the unit's image is placed at (required) |
| `--no-lift` | tier 0: one statement per instruction |
| `--abi apcs` | the unit follows the APCS, as compiler output does, so floating-point registers f1-f3 may be treated as dead at a return |
| `--poll-loops` | make every loop a point where the runtime may do its background work; for an interpreter, so that Escape can stop a program's loop |
| `--swis file` | SWIs the runtime implements natively, one per line (`0x0001E OS_Module`); compiled code calls them directly |
| `--swi-regs file` | the registers each SWI reads and writes, as bit masks, one per line (`0x0001E in=0x0009 out=0x0005 OS_Module`); without it a SWI is taken to read and write every register, which leaves the lifted code less to optimise |

### What the C needs

The C is written against the BOX runtime's CPU header, `rosgd/cpu.h` (and
`rosgd/error.h`). These are not in this repository yet. They provide:

- `struct ros_cpu`: the register block -- `r[16]`, the flags `n z c v`
  (each 0 or 1), the mode, and a pointer to the floating-point state;
- the inline helpers the code uses: shifts, flag-setting arithmetic,
  condition tests, loads and stores (`ros_ld32`, `ros_st32` and so on),
  and FPA and VFP conversions;
- `ros_swi(s, number)`, which carries out a SWI;
- `struct ros_code_entry` and `ros_code_register()`, which record where
  each compiled entry point is, so that a branch to an address finds its
  C function.

A host that supplies those can run the C.

## More

[STATUS.md](STATUS.md) says where the assembler stands and how it is
checked against the DDE's own ObjAsm. The scripts in `tools/` are those
checks; they expect the RISC OS sources and an emulator, and are not
needed to use the assembler.
