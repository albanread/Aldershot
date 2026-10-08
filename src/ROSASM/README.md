# ROSASM

An ObjAsm-compatible assembler for RISC OS 5, written in Rust. It reads
ObjAsm source -- the language the RISC OS 5 sources are written in -- and
writes AOF object files for the RISC OS linker, or ELF32 objects with
`--elf`. The code it assembles is 32-bit ARM (AArch32).

Experimental software, unsupported. MIT licence: see [LICENSE](LICENSE).

## Building

You need Rust (`cargo`). There are no other dependencies.

```
cargo build --release
```

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

`rosasm` with no arguments prints the full usage, including the `--emit c`
form, which compiles a unit to C instead of assembling it. That form is
used to build the BOX and needs the rest of its tree.

## More

[STATUS.md](STATUS.md) says where the assembler stands and how it is
checked against the DDE's own ObjAsm. The scripts in `tools/` are those
checks; they expect the RISC OS sources and an emulator, and are not
needed to use the assembler.
