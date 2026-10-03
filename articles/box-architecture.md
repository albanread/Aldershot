# BOX Architecture

*3 October 2026*

## Introduction

BOX is RISC OS 5 running on a Linux kernel, with RISC OS itself translated
to C and compiled as native code. This article describes how the system is
put together as it stands today:

* what the Linux kernel provides, and what RISC OS provides
* the arena, which is the memory both of them share
* how RISC OS tasks are run as threads
* how a SWI is made.

Notation follows the usual RISC OS conventions. Hexadecimal numbers are
prefixed with an ampersand, as in `&8000`. Registers are R0 to R15, SWIs
are written as `OS_WriteC`, and * Commands as `*Run`.

---

## 1. Overview

The system has three parts:

* the Linux kernel
* the RISC OS *personality*, which is RISC OS 5 translated to C
* the *arena*, the low 4 GB of the address space, in which all RISC OS
  memory lives.

```
        0                                   4 GB
        +-------------------------------------+
        |              the arena              |
        |  application slot, RMA, screen,     |
        |  ROM areas, zero page               |
        +-------------------------------------+
          set up and used by     backed by
          the personality        the kernel
```

*Figure 1. The personality and the kernel both see the arena. A RISC OS
address is used directly as a host address.*

The kernel does the jobs it would do on any Linux system: it manages
memory and threads, and it drives the devices and filing systems. The
personality has no device drivers or filing systems of its own. When RISC
OS needs to read a disc or draw on the screen, the personality calls the
kernel to do it.

No data is converted or copied as it passes between the two; see
section 4.

---

## 2. The Linux kernel

The kernel is an unmodified Linux kernel. The personality runs as the
first process, `/init`, and everything else in the box is done by `/init`
and its threads.

`/init` must be built as a static PIE (position-independent executable).
A static executable that is not position-independent is loaded at 16 MB,
which is inside the arena, and the personality would then be unable to
reserve the arena for itself. A PIE is loaded well above 4 GB, out of
reach of any RISC OS address. There are no shared libraries in the box:
the personality and all the RISC OS modules it contains are linked into
the one executable.

At start-up `/init` mounts `devtmpfs`, `proc` and `sysfs`, and sets
`vm.mmap_min_addr` to `&4000`. The kernel configuration already allows
mappings at `&8000`, where application space starts, but ScratchSpace is
at `&4000`, so the limit has to come down further.

Devices are all handled by the kernel. The screen is a framebuffer
supplied by the kernel and mapped into the arena at `&A0000000`. Storage,
input and sound are likewise kernel services which the personality calls.
If a program faults, the kernel passes the fault to the personality, which
turns it into the RISC OS error the program would expect. The box carries
on running.

### Single processor

All threads of the personality are tied to one CPU, the one on which
`/init` started. Only one task runs at any time (see section 5), so a
second CPU would gain nothing, and waking a thread on another CPU costs a
good deal more. Measured on the development virtual machine:

| Waking a thread | Time |
| --- | --- |
| on the same CPU | about 6 µs |
| on a different virtual CPU | about 58 µs |

---

## 3. The personality

The personality is RISC OS 5.31, translated to C from the RISC OS Open
sources and compiled for the host processor. It is compiled with 32-bit
pointers, so that a C pointer and a RISC OS address are the same thing.
32 bits is enough, since every RISC OS address lies in the arena.

The RISC OS modules (the Kernel, FileSwitch, the Window Manager, the Font
Manager, BASIC and so on) are translated in the same way and linked into
`/init`. Module code is therefore never loaded at run time; it is already
present. The ROM is a region of the arena at `&FC000000`, laid out with
each module's code and static data. Programs that look at the ROM find
what they expect there.

All compiled code is entered in the same way. An entry point is a C
function of the form:

```c
void ros_code(struct ros_cpu *cpu);
```

`struct ros_cpu` holds the register state:

* R0 to R15, all as arena addresses (R13 is the stack pointer, R14 the
  link register and R15 the program counter)
* the N, Z, C, V and Q flags, each held as a separate integer
* the processor mode and I bit, as modelled
* a pointer to the task's floating point state.

The same structure is used at every boundary in the box.

---

## 4. The arena

The arena is the bottom 4 GB of the address space. Everything a RISC OS
program can see is in it. The rule is simple:

> A RISC OS address is a host address.

Nothing is translated or copied. If one side writes a sprite, a file name
or a Wimp message block at `&1C8C00`, the other side reads it at
`&1C8C00`. A RISC OS address is 32 bits, so it cannot refer to the
personality's own memory above 4 GB. If a pointer to non-arena memory
ever reaches compiled code, the box stops and reports it; that is a bug in
the box, not in the program.

The first thing the personality does is reserve the whole range from
`&8000` to 4 GB as one inaccessible mapping, so that the kernel cannot
place anything at a RISC OS address. The individual regions are then
mapped over the reservation at fixed addresses:

| Address | Region | Size | Contents |
| --- | --- | --- | --- |
| `&00004000` | ScratchSpace | 16 KB | Kernel scratch space |
| `&00008000` | Application space | slot size | Current task's program and data |
| `&20000000` | RMA | up to 256 MB | Module code and workspace |
| `&30000000` | System heap | up to 32 MB | System allocations |
| `&32000000` | SVC stack | 1 MB | Stack used by compiled code |
| `&A0000000` | Screen | depends on mode | Screen memory |
| `&FC000000` | ROM | 48 MB | Compiled modules |
| `&FFFF0000` | Zero page | 32 KB | Kernel workspace |

Dynamic areas are mapped into the reservation as they are created. Zero
page holds the kernel's public workspace at the usual offsets: for
example `MetroGnome` (the centisecond counter) at `+&10C`, `ReturnCode`
at `+&AC4`, and the current Wimp task's domain at `+&FF8`.

Each region is backed by a memfd (a named Linux memory object with a file
descriptor) rather than by private memory. This matters for three reasons:

1. A region can be mapped again at the same address. This is how one
   task's application slot replaces another's when the Wimp switches
   tasks. It would also allow the regions to be shared with other
   processes, should that ever be needed.
2. A slot is grown by extending its memfd, in the same way that the RMA
   grows.
3. Code runs from the application slot and the RMA, as it does on RISC
   OS, so their memfds are created executable. Permission for this is
   obtained from the kernel once at start-up, not for each program.

---

## 5. Tasks

Each RISC OS task is a thread of `/init`. A task is created with its
slot, an entry point and an argument. The entry point is a function in the
personality which runs the task's program until it exits. There is
exactly one thread per task.

Two mechanisms control which thread may run: the personality lock and
the baton.

### The personality lock

The personality lock is a single mutex. A task's thread holds it while
running and releases it when it waits.

Module code runs in the context of its caller. When a task calls a SWI,
the SWI handler runs on the task's own thread and under the lock the task
already holds. Calls are never passed to another thread, and no other
thread touches the personality's state, so there are no races to guard
against.

### The baton

The baton is the right to run in the desktop. It is implemented as a
flag for each task, protected by one mutex.

The Wimp switches tasks in `Wimp_Poll`, as it always has. The switch is
done by the thread of the task that is giving up the baton, while it
still holds the personality lock. That thread saves its own task's state
and puts the next task's state in place: the slot mappings, the
personality's internal pointers and zero page. It then releases the lock
and waits on its own flag. When the next task's thread wakes up,
everything is as that task left it.

The result is that a task is paged out at the point RISC OS would page it
out, and blocks where RISC OS would block, inside the SWI. A SWI that
waits (an idle poll, a sleep, a slow transfer) releases the lock while it
waits. The thread sleeps inside the SWI, other work carries on, and when
the wait is over the SWI returns to the caller on the same thread.

### Callbacks

The personality keeps count of SWI nesting for each task. Exit from a
task's outermost SWI is treated as a safe point, and transient callbacks
for the task are delivered there and nowhere else. A program that
computes for a long time without calling RISC OS will therefore see its
callbacks late. This is the one difference from interrupt-time delivery
on real hardware, and the delay is bounded by the program's next SWI.

---

## 6. SWIs

In BOX a SWI is an ordinary function call. There is no SWI instruction to
trap. Translated code and C programs call the dispatcher directly:

```c
void ros_swi(struct ros_cpu *s, uint32_t number);
```

`s` holds the registers and `number` is the SWI number, including the X
bit. The call is made on the task's own thread, with the personality lock
held. It costs about the same as any other function call, around 0.1 µs.

The SWI number selects the handler:

| SWI numbers | Handled by |
| --- | --- |
| `&00` to `&FF` | the Kernel's own SWIs, using their translated code |
| `&100` to `&1FF` | `OS_WriteI`, with the character in the low byte of the number |
| all others | the module that owns the SWI chunk |

A module's SWI handler is entered just as RISC OS enters it: R11 holds
the offset within the chunk, R12 points to the module's private word, and
R10 to R12 are preserved for the caller. `OS_CallASWI` and
`OS_CallASWIR12` are supported; the SWI given in R10 or R12 is called in
their place, and its own X bit decides how errors are handled.

The X bit is bit 31 of the number, as usual:

* If the X bit is set and an error occurs, V is set on return and R0
  points to the error block. The error block is in the arena, so the
  caller can read it directly.
* If the X bit is clear, the error is raised. The personality enters the
  task's error handler with the registers, mode and stack that the RISC
  OS 5.31 Kernel would use. For a translated task the error handler is
  itself translated code, and it is entered in the same way as any other
  handler.

---

## 7. Summary of design rules

1. A RISC OS address is a host address. Data is never converted or copied
   between RISC OS and the host.
2. The kernel looks after the hardware and the personality looks after
   RISC OS.
3. There is one program. There are no shared libraries, no separate OS
   process, and no forwarding of calls between threads.
4. One processor and one lock. Only one task runs at a time, on its own
   thread.
5. A SWI is a function call, with no trap.

Most of these rules exist to avoid work. Rule 1 saves a copy at every
interface, rules 3 and 4 remove a thread or process boundary from every
call, and rule 5 removes a trap from the most heavily used path in RISC
OS.
