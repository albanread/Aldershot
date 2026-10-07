# POSIX applications: RISC OS programs on the Unix underneath

Design 22 (RISCOSGrandDesign `docs/design/22-unix-bridge.md`). A POSIX
application is x32 code in the arena, run by the C applications' loader
(`include/rosgd/capp.h`) as any design 20 application is: a RISC OS task,
with SWIs and the Wimp. Its C library is **musl**, and musl's system calls go
to Linux through the **Unix bridge**: the UnixBridge module
(`modules/unixbridge`, SWI `Unix_Syscall` &C0100).

## Build and run

    make posix-libc       musl for x32, with this directory's overlay
    make posix-tests      tests/posix/*.c, linked at &8000
    make posix-boxcheck   the tests run in the box, their output checked

A program is compiled against `$(POSIX)/include` with `POSIX_CC`
(posix.mk) and linked by `link.py`. It is an ordinary ELF32 x86-64 file at
&8000 with the ROSGD note and OS/ABI 255, filed as an Absolute (&FF8):
`*Run` runs it.

A Unix package's own build uses `posix/cc` (with `posix/ar` and
`posix/ranlib`) as its compiler. It compiles for x32 and links with
`link.py`, taking `-L`/`-l` as static archives and dropping what only a
dynamic or GNU link needs. `$ROSGD_LIBPATH` and `$ROSGD_CFLAGS_EXTRA` let a
port add library directories and compile options (`ports/netsurf/build.sh`).

## What is here

| File | What |
|---|---|
| `build_musl.py` | builds `libc.a` from zig's bundled musl 1.2.5, as musl's own Makefile would, with the overlay; adds what zig's copy leaves to zig (lib/c's string functions, compiler_rt's `mem*` and maths), built from zig's sources for x32 |
| `musl/` | the overlay: the only files of musl that change |
| `../abi/x32/crt/crt1.c` | `_start`: `UNIX_ROSGD_INIT`, a new stack in the program's heap area, then argv, the environment and an auxiliary vector for `__libc_start_main` |
| `../abi/x32/crt/gate.c` | `__rosgd_syscall` (SWI `XUnix_Syscall` through the C applications' SWI gate) and `__rosgd_swi` (any SWI) |
| `../abi/x32/crt/switch.s`, `note.s` | the stack switch, and the ROSGD note |
| `cc.py` (`cc`), `ar`, `ranlib` | the compiler, archiver and ranlib a Unix package's build runs |
| `riscos/` | `<kernel.h>` and `<swis.h>` (U-g): `_swix`, `_swi`, `_kernel_swi` and SharedCLibrary's OS calls, in `libc.a` (`kernel.c`); `swis.py` puts RISC OS's SWI names into `swis.h`, then ROSGD's from api/defs |
| `posix.ld`, `link.py` | the image's layout; the link, OS/ABI 255, and the loader's checks, plus a check that no instruction uses `%fs` or `%gs` |

## The Apple Silicon box: A64X32 (design 22 B8, design 26)

    make ARCH=aarch64 posix-libc posix-tests posix-boxcheck

`build_musl_a64x32.py` builds upstream musl 1.2.6 (zig has no A64X32) with
the pinned clang (`.cache/llvm`) and A64X32's flags, into
`build/aarch64/gen/posix`; its file `abi` says `a64x32`, and `cc.py` and
`link.py` read it. Programs are linked by **roscc** (ld.lld has no P32
relocations), so every object passes roscc's x18 and addressing checks.

- **The arch** (`musl/arch/a64x32`): aarch64's types at ILP32's widths,
  `long double` as `double` (A64X32's table), atomics by the compiler's
  builtins (upstream's LL/SC takes 64-bit operands), the thread pointer a
  variable (x18 is the static base, TPIDR_EL0 the runtime's).
- **The system-call numbers are x32's**, and so are the calls
  (`syscall_arch.h` includes x32's): they are the bridge's own numbering
  (`include/rosgd/unixbridge.h`), not a kernel's. The bridge's x32
  conversions (iovec, msghdr) are ILP32's, so they serve A64X32 unchanged.
  What differs is what the runtime's arm64 libc writes without conversion:
  `struct stat` (`kstat.h`, arm64's layout) and the `O_` flags
  (aarch64's `bits/fcntl.h`).
- **Sources**: upstream's; its aarch64 C (the maths) but never its
  assembler, which forms 64-bit addresses; upstream's x32 C where the x32
  meaning is the point (itimerval, mq_attr); the overlay's gate files in
  `src/*/x32` (machine-neutral C); and `src/*/a64x32`: setjmp, longjmp
  and sigsetjmp (the jmp_buf's address zero-extended; sigsetjmp's saved
  x30 and x19 moved past ILP32's mask), fenv and dlsym in C, sysinfo.
- **The start files** are x32's C (`abi/a64x32/crt` includes them); only
  the stack switch is A64. There is no note.o: roscc writes the note.

`roscc a64x32-lint` over the library finds 3 unbounded accesses of
18,018, in `__res_msend_rc`, `ecvt` and `fcvt`; a program that takes one of
them is refused by the link.

## The overlay

- `arch/x32/syscall_arch.h`: every `__syscallN` calls `__rosgd_syscall`, an
  ordinary out-of-line function. There is no `call` in inline assembler,
  which would clobber the red zone behind the compiler's back.
- `arch/x32/pthread_arch.h`, `src/thread/x32/__set_thread_area.c`: the
  thread pointer is a variable, not `%fs`, which is the runtime's. It is
  per task, because a task's memory is at &8000 while its code runs.
- `src/thread/x32/{clone,syscall_cp,__unmapself}.c`,
  `src/process/x32/vfork.c`, `src/signal/x32/restore.c`,
  `src/unistd/x32/lseek.c`: upstream's inline `syscall`s, through the gate.
  There are no threads yet (`clone` is refused), and `vfork` is refused.
- `src/env/getenv.c`: a name the environment lacks is read from the system
  variable of that name (design 22, U-c).

## RISC OS from C

`<swis.h>` and `<kernel.h>` are RISC OS's C interface to the OS, written
fresh (design 22, U-g); a GCCSDK port's SWI calls compile unchanged.
- `_swix(swi, mask, ...)` and `_swi`, with RISC OS's masks: `_IN`, `_INR`,
  `_OUT`, `_OUTR`, `_FLAGS`, `_RETURN`, `_BLOCK` (at most 16 words).
- `_kernel_swi`, `_kernel_swi_c`, `_kernel_last_oserror`, the `_kernel_os*`
  calls, `_kernel_getenv` and `_kernel_setenv`, `_kernel_command_string`.
- The SWI names are those RISC OS's own `<swis.h>` has: `swis.py` takes them
  from the OS's headers, as its build's makehswis does (tools/capps.py),
  from the sources at `$(BASICVPFSRC)`, then adds ROSGD's from api/defs.
  So `make posix-libc` needs those sources.

Each SWI goes through the C applications' gate: an x32 pointer is the
address RISC OS sees. There is no OSLib (design 22, section 11).
`tests/posix/swis.c` checks the layer in the box.

## The rules

- **No thread-local storage, and no `%fs` or `%gs` in any instruction.**
  `link.py` refuses both. Keep `-fno-stack-protector`: the canary is read
  from `%fs`.
- **Memory is a dynamic area of the program's own,** named and sized by
  `__dynamic_da_name` and `__dynamic_da_max_size` if the program defines
  them, as UnixLib's programs do. It holds the stack, then brk's reach, then
  what mmap gives. It is never application space, which each task switch
  remaps whole.
- **Paths** may be RISC OS names (`HostFS::Disc.$.a.b`, `<X$Dir>.y`,
  `Choices:z`), which FileSwitch resolves to their Linux files, or Unix
  paths. A relative Unix path is from the program's own current
  directory, which starts as the CSD.
- **Descriptors 0-2 are the console**: the VDU and the keyboard, `\n` as
  OS_NewLine. A command tail may redirect them as UnixLib's programs'
  may: `<file`, `>file`, `>>file`, `2>file`, `2>>file`, `2>&1`.
- **`exit` ends the program**, and its status is the return code
  (`Sys$ReturnCode`).
