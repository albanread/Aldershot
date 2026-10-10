# BoxTools

A native module that lets you write software in the box. It adds commands to
compile C, BBC BASIC V and Mojo programs, and to run Linux programs. Builds can
be driven by Obey files.

## Commands

| Command | What it does |
|---|---|
| `*CC <source>... [-o <program>] [options]` | Compiles and links C. Both boxes use clang. Hosted, it uses the Mac's tcc and makes Linux programs only. |
| `*CC --linux <source>...` | Makes a Linux program (static, musl, typed ELF &E1F) instead of a RISC OS application |
| `*RunBox <program> [arguments]` | Runs a Linux program. Its exit status goes to `Sys$ReturnCode`. |
| `*RosBas <source> [-o <program>] [-L <dir>]... [--quirk <name>]...` | Compiles a BBC BASIC V program with rosbas into an application (typed &FF8) |
| `*RosBasRun <source> [-o <image>] [--no-run]` | Compiles with rosbas and runs the result at once |
| `*Mojo [--kernel] <source> [-o <program>]` | Compiles a Mojo program into an application, or a Worker kernel with `--kernel` |
| `*RosAsm <source> [options]` | Runs rosasm on RISC OS names. Hosted only. |

Sources are RISC OS names. `c.hello` makes `hello` beside the `c` directory,
and `hello/c` does the same. A source named another way needs `-o`. A program
that would overwrite one of its sources is refused. Applications link against
the ROM's SharedCLibrary, so `-l` and `-L` are refused for them.

`Tcc$Exe`, `RosAsm$Exe`, `Mojo$Exe` and `Clang$Exe` name other programs as Linux
paths. BoxTools sets the run action for ELF files to `*RunBox`, and sets
`File$Type_A6E` to `Mojo`.

Errors are &C0120 + n: 0 syntax, 1 could not run, 2 no program name or it would
overwrite a source, 3 did not compile, 4 not a program, 5 no rosasm or mojo
here, or `*RosBasRun` used hosted.

## How programs run

clang, tcc, mojo and the programs `*CC --linux` makes run as Linux child
processes on a pseudo-terminal, as the PTY module runs `ssh`. Their output is
drawn in the text window and they read the RISC OS keyboard. If one crashes,
only the child ends.

## Differences from RISC OS 5.30

None of these commands exist on RISC OS. `*RosBasRun` uses rosbas's JIT in the
Apple Silicon box. The Intel box has no JIT, so it builds the image and then
runs it. `*RosBasRun` and the x32 code are not available hosted. `*RosAsm` is
refused in the box, which has no rosasm.

The box's clang, musl, rosbas, roscc and Mojo come from the initramfs.
`deps/build-clang-box.sh`, `deps/get-clang-box.sh`, `deps/get-mojo.sh` and
`deps/build-mojo-box.sh` fetch or build them. Mojo's standard library is
precompiled, and the `riscos` and `demos` packages are staged as source.

## Tests

`boot/selftest_boxtools.c`, hosted and in the box. It covers program naming,
`*CC` and `*RunBox`, the refusals, `*RosAsm`, `*RosBas`, `*CC --linux` and
`*Mojo`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
