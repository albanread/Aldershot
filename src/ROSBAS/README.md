# ROSBAS

A compiler for BBC BASIC V, written in Rust. It reads a BASIC program, as
text or tokenised, and writes it out as C. The language it compiles is RISC
OS 5.30's BBC BASIC (BASICVFP), including its quirks.

Experimental software, unsupported. MIT licence: see [LICENSE](LICENSE)
and [NOTICE](NOTICE).

## Building

You need Rust (`cargo`). There are no other dependencies.

```
cargo build --release
cargo test
```

The program is `target/release/rosbas`.

## Using it

```
rosbas list FILE
rosbas tokenise FILE OUT
rosbas compile FILE [-o OUT.c] [-L DIR]... [--quirk NAME]... [--quirks all]
rosbas build FILE [-o EXE] [-L DIR]... [--quirk NAME]... [--quirks all]
```

- `list` prints a program as BASIC's `LIST` would.
- `tokenise` reads a program written as text and writes it tokenised, as
  BASIC's `TEXTLOAD` does.
- `compile` writes the program as one C file.
- `build` compiles the program and builds it for this machine with the
  host's C compiler.

`-L` names a directory to search for `LIBRARY` files. `--quirk` turns on
one of the interpreter's quirks that the compiler leaves off by default.

For example, with `hello.bas`:

```
10 FOR I%=1 TO 3
20 PRINT "Hello ";I%
30 NEXT
```

```
rosbas tokenise hello.bas hello,ffb
rosbas list hello,ffb
rosbas compile hello.bas -o hello.c
```

The C that `compile` writes uses the ROSBAS runtime library, through
`rb_gen.h`. The runtime is not in this repository yet, so `build` and the
C it produces cannot be used from here alone. `list`, `tokenise` and
`compile` work as they are.

## How it works

- `textload.rs` tokenises text exactly as BASIC's `TEXTLOAD` does.
- `parse.rs` reads each statement as the interpreter does.
- `flow.rs` works out the program's control flow once, at compile time,
  where the interpreter scans for it as it runs.
- `lower.rs` turns the statements into a simple intermediate form
  (`ir.rs`), and `cgen.rs` writes that as C.
- `driver.rs` ties the steps together for the command.

See [STATUS.md](STATUS.md) for how the compiler is tested and where it
stands.
