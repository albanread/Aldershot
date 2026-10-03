# UnixBridge

*3 October 2026*

## Introduction

UnixBridge lets a RISC OS program make Linux system calls. It is how
software written for Unix, and ported to RISC OS, runs on BOX with the
real Unix underneath it, rather than with Unix imitated out of RISC OS
SWIs.

UnixBridge is specific to BOX. It exists because BOX runs on a Linux
kernel. RISC OS on other machines has no such thing, and a program that
uses it will run only on BOX.

This article describes where UnixBridge fits, how it works, what it can
be used for, and what it does not do.

---

## 1. Three kinds of program

BOX runs three kinds of program, and UnixBridge is what makes the third
possible.

| | RISC OS application | POSIX application | Linux program |
| --- | --- | --- | --- |
| Runs | in the arena, as a RISC OS task | in the arena, as a RISC OS task | as its own Linux process |
| Pointers | 32-bit | 32-bit | 64-bit |
| C library | the ROM's SharedCLibrary | musl, through UnixBridge | musl |
| SWIs and the Wimp | yes | yes | no |
| Unix system calls | no | yes, through UnixBridge | yes, directly |

* A **RISC OS application** is an ordinary RISC OS program: SWIs, the
  Wimp, and the ANSI C library.
* A **Linux program** (`*CC --linux`, run by `*RunBox`) is a normal
  Linux process on a pseudo-terminal. It has all of Unix, but no RISC OS:
  it cannot make a SWI or open a window.
* A **POSIX application** has both. It is a RISC OS task, in its own
  slot, able to make SWIs and be a Wimp task. Its C library is musl, the
  same small Linux C library BOX's runtime uses, and musl's system calls
  go to Linux through UnixBridge.

---

## 2. Why not UnixLib

On RISC OS, Unix software is ported with GCCSDK and UnixLib. UnixLib has
to imitate Unix with what RISC OS provides: files through FileSwitch
with names translated, sockets through the Internet module, `fork` as a
child task, signals from Escape and error handlers. It covers what ports
have needed, one call at a time.

BOX is different. Underneath it is a real Unix kernel, and a real Unix
C library. So rather than imitate Unix, UnixBridge passes a program's
system calls to Linux itself.

---

## 3. How it works

UnixBridge is a native module with a single SWI:

| SWI | Number | Entry | Exit |
| --- | --- | --- | --- |
| `Unix_Syscall` | `&C0100` | R0 = the system call's number; R1 = the address of its six 64-bit arguments | R0, R1 = the result, low and high words; a negative value is `-errno` |

A POSIX application's copy of musl is changed in one place: where it
would execute a system call instruction, it calls `Unix_Syscall` through
the ordinary SWI gate instead. The rest of musl is untouched.

UnixBridge then deals with each call in one of four ways:

* **Pass it to Linux.** Most calls go straight through. The program's
  pointers need no change, because in BOX a RISC OS address is a host
  address. File descriptors go through the program's own table, and
  relative paths are resolved from the program's own current directory.
* **Convert it.** A few calls carry structures whose layout differs
  between 32-bit and 64-bit code, such as the `iovec` arrays of `readv`
  and `writev`. These are converted in and out.
* **Make it the task's own.** Some things belong to a whole Linux
  process, and BOX is one process. These are kept per program instead:
  its descriptor table, current directory, umask, process id, memory and
  exit. `exit` ends the program with `OS_Exit` and sets
  `Sys$ReturnCode`; it does not end BOX.
* **Refuse it.** Anything not on UnixBridge's list is refused with
  `ENOSYS`, and reported once on the console, so a missing call shows
  itself.

Refusal is the default. A call is passed through only if it is known to
be safe for one program to make on behalf of the whole box.

### Files, memory and the console

* **Files.** A program can use Unix paths or RISC OS ones. A HostFS disc
  is a Linux directory, so both reach the same files.
* **Memory.** A POSIX program's stack, heap and `mmap` space come from a
  dynamic area of its own, never from application space, and never from
  outside the arena.
* **The console.** Descriptors 0, 1 and 2 are the VDU and the keyboard.
* **Waiting.** A call that waits, such as a read or a `poll`, lets other
  RISC OS work run while it waits, as BOX's own modules do.

---

## 4. Not only C

Because UnixBridge is a SWI, anything that can make a SWI can use it,
not just C programs built with musl. A BBC BASIC program can make a Linux
system call directly:

```
SYS "Unix_Syscall", nr%, block%
```

UnixLib never offered this.

---

## 5. What it is used for

### Porting Unix software

The main use is porting software written for Unix, where the program
wants both Unix facilities and the RISC OS desktop. A POSIX application
is built with BOX's POSIX compiler wrapper (`posix/cc`), so a Unix
package's own build can usually be used as it stands. `<kernel.h>` and
`<swis.h>` are provided, so the program can also make SWIs with `_swix`
and `_kernel_swi`, as RISC OS C programs do.

### NetSurf

NetSurf is the first, and the test case. It runs on BOX as one POSIX
application with two halves:

* **The Unix half** is NetSurf's core and its libraries, unmodified,
  built against musl. Its file and memory calls go through UnixBridge.
* **The Wimp half** is a new RISC OS front end: windows, toolbar,
  menus, icon bar.

NetSurf fetches web pages through RISC OS's own URL_Fetcher and
AcornHTTP (see *BOX on the Network*), so it needs no network code of its
own. It runs on both the Intel and Apple silicon boxes.

### Programs that need Linux directly

A RISC OS program, in any language, can reach a Linux facility that has
no RISC OS equivalent, through `Unix_Syscall`, without a module being
written for it first.

---

## 6. What it does not do

* **No `fork`.** A RISC OS task cannot be copied into a new Linux
  process. `fork` and `vfork` are refused, as UnixLib's were in effect,
  and programs ported with GCCSDK already cope with that.
* **No threads yet.** `clone` is refused. A program may not use
  thread-local storage, and is refused at link time if it does.
* **Nothing that would affect the whole box.** Loading kernel modules,
  `mount`, `reboot`, changing user, tracing other processes, and calls
  that would change `/init`'s own signal handling or reap other modules'
  children are refused.

---

## 7. Specific to BOX

UnixBridge is not part of RISC OS, and is not a portable interface:

* `Unix_Syscall` is in BOX's own SWI range (`&C0100`). On RISC OS it
  does not exist.
* Its call numbers and structures are Linux's.
* POSIX applications are linked for BOX, and are not RISC OS programs
  that run elsewhere.

A program that must also run on RISC OS should keep to RISC OS's own
interfaces, or be built twice, once with UnixLib for RISC OS and once
against UnixBridge for BOX.
