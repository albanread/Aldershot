# BOX Design

*8 October 2026*

## Introduction

BOX is RISC OS 5 running on a Linux kernel, with RISC OS itself translated
to C and compiled as native code. This article describes how the system is
put together as it stands today:

* what the Linux kernel provides, and what RISC OS provides
* the arena, which is the memory both of them share
* where the two meet, service by service
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

No data is converted or copied as it passes between the two. Section 5
describes each of these interfaces in turn.

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

At start-up `/init` mounts `devtmpfs`, `proc` and `sysfs`, and lowers
`vm.mmap_min_addr` to one page, `&1000`. Linux will not normally map
anything in the first 64 KB, which is a sensible precaution on a general
purpose machine but is exactly where RISC OS keeps two of its own
regions: DebuggerSpace at `&2000` and ScratchSpace at `&4000`. The limit
comes down to one page and no further, so that page zero stays unmapped
and a null pointer followed in the box's own code still faults where it
is made.

Devices are all handled by the kernel. The screen is a framebuffer
supplied by the kernel and mapped into the arena at `&B4000000`. Storage,
input and sound are likewise kernel services which the personality calls.
If a program faults, the kernel passes the fault to the personality, which
turns it into the RISC OS error the program would expect. The box carries
on running.

### Single processor

All threads of the personality are tied to one CPU, the one on which
`/init` started. Only one task runs at any time (see section 6), so a
second CPU would gain nothing, and waking a thread on another CPU costs a
good deal more. Measured on the development virtual machine:

| Waking a thread | Time |
| --- | --- |
| on the same CPU | about 6 µs |
| on a different virtual CPU | about 58 µs |

This applies to RISC OS's own work. Linux helper threads — the display
converter, the sound filler, the Worker pool — are not tied down, and run
on the other processors (section 5.10).

---

## 3. The personality

The personality is RISC OS 5.31, translated to C from the RISC OS Open
sources and compiled for the host processor. It is ordinary 64-bit code
(LP64), loaded above 4 GB, and it never holds a RISC OS address as a C
pointer. It reaches the arena through accessors such as `ros_ld32()`.
Because the arena is mapped at address 0, each of these compiles to a
single load or store.

RISC OS's C programs are compiled differently. The ROM's SharedCLibrary,
C modules, C applications and anything built in the box are compiled
with 32-bit pointers (x32 on Intel, A64X32 on Apple silicon), so for
them a C pointer and a RISC OS address are the same thing. They reach
the system only through the SWI gate, a fixed page in the arena at
`&FEEFF000`. *BOX Tools* describes this in more detail.

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
| `&00000000` | Compatibility page | 4 KB | Not mapped in BOX (see below) |
| `&00002000` | DebuggerSpace | 4 KB | The Debugger module's page |
| `&00004000` | ScratchSpace | 16 KB | Kernel scratch space |
| `&00008000` | Application space | slot size, up to 1.5 GB | Current task's program and data |
| `&60000000` | RMA | up to 256 MB | Module code and workspace |
| `&70000000` | System heap | up to 32 MB | System allocations |
| `&72000000` | SVC stack | 1 MB | Stack used by compiled code |
| `&72200000` | C ROM | to `&78000000` | The ROM's C library |
| `&78000000` | Dynamic areas | up to the screen | Allocated upwards |
| `&B4000000` | Screen | up to 64 MB | Screen memory |
| `&FC000000` | ROM | 48 MB | Compiled modules |
| `&FEEFF000` | SWI gate | one page | Entry to the OS for x32 code |
| `&FFFF0000` | Zero page | 32 KB | Kernel workspace |

The gaps are as real as the regions. `&1000` to `&2000` and `&3000` to
`&4000` are not mapped, and reading them faults, exactly as it does on
RISC OS 5.30.

RISC OS 5 can keep a read-only *compatibility page* at address 0, filled
with a pattern, for the benefit of old software that reads low memory by
mistake. BOX leaves it off, so that a null pointer followed in the box's
own translated code faults where it is made rather than quietly reading
zeros. `OS_Memory 20` reports the page as absent and refuses to put it
there, which is how RISC OS itself answers when it cannot.

The second page of the SVC stack is left inaccessible as a guard, so that
code running away down the stack faults there rather than running into
the system heap below it.

Dynamic areas are mapped as they are created, from `&78000000` upwards.
Zero page holds the kernel's public workspace at the usual offsets: for
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

Application slots are lazy. A task is given the whole 1.5 GB of address
space and the pages become real memory only as they are touched. If the
machine runs out, the kernel reports a bus error on the page, which the
personality turns into a data abort for the program; the box itself lives
on.

---

## 5. Where Linux meets RISC OS

This section describes the interfaces one at a time. They have a common
shape, and it follows from the rule in section 4. Because a RISC OS
address is a host address, an interface never has to marshal anything: it
passes a RISC OS address to Linux, or maps what Linux gives it into the
arena at a RISC OS address, and both sides then work on the same bytes.

The whole of it is a small layer, declared in one header
(`include/rosgd/platform.h`) and about a dozen files. RISC OS's own
modules sit on top of it and keep their usual interfaces, so a program
sees the SWIs it has always seen.

| RISC OS facility | Served by |
| --- | --- |
| Screen memory, modes | A DRM framebuffer, mapped into the arena |
| The mouse pointer | DRM's cursor plane, or drawn into the frame |
| Keyboard, mouse, tablet | Linux input devices (evdev) |
| Sound | ALSA, or CoreAudio in the Mac application |
| Filing systems | HostFS over the Linux filing systems |
| The network | Linux sockets, and libcurl and OpenSSL above them |
| Time | The monotonic clock |
| Aborts and errors | Linux signals, turned into RISC OS errors |
| Tasks | POSIX threads |
| Reset and power off | `reboot()` |

### 5.1 The screen

Screen memory is a DRM dumb buffer. The personality asks the kernel for
it, and maps it into the arena at `&B4000000`. From that moment the VDU
drivers write pixels into it with ordinary stores, in the RISC OS mode's
own format — 1, 2, 4, 8, 16 or 32 bits per pixel. There is no conversion
on the drawing path, and nothing to flush: a program that pokes screen
memory directly, as BBC BASIC programs have always done, is writing to
the display.

The last 4 KB of the buffer holds a *screen block*, which describes the
mode to whatever is showing it: size, depth, row length and the palette.
Three paths use it:

* **Direct scan-out.** On hardware, or on the Apple silicon box, the
  buffer is the one the display controller reads. Nothing copies
  anything.
* **Conversion.** Some displays cannot show a 4 bits-per-pixel RISC OS
  mode. There a thread of the platform's own turns screen memory into
  the display's XRGB8888, scaled and centred, and flushes it. Callers
  see no difference.
* **The compositor.** In the WPE edition a Wayland compositor owns the
  display, and RISC OS's screen is its bottom layer, with Linux
  programs' windows composited over it as Wimp windows.

Mode changes go the same way. `OS_ScreenMode 2` enumerates what the
display offers, which is the DRM connector's mode list with duplicates
removed, so the modes RISC OS offers are the ones the monitor really
has.

### 5.2 The pointer

The mouse pointer is DRM's cursor plane where there is one: the shape is
handed to the kernel and the host draws it, so moving the pointer does
not disturb the screen underneath and costs nothing to redraw. Where
there is no cursor plane — a virtual machine window, for instance — the
pointer is drawn into the frame by the same converter thread, and taken
out again before the next conversion.

### 5.3 Keyboard and mouse

Input comes from Linux's input devices, evdev, which on a virtual
machine are virtio's keyboard and tablet and on a PC are the real ones.
A pump watches them all.

The platform turns each Linux event into RISC OS's terms and delivers it
at a safe point, holding the personality lock. A key becomes RISC OS's
own low-level key number — the numbering in the kernel's `hdr/Keyboard`,
which is the Archimedes keyboard's, not Linux's — and mouse buttons
become key numbers too, as RISC OS has always treated them. Movement is
relative; a tablet reports its position as a fraction of its range; the
wheel reports notches.

What RISC OS then does with the event is the Input module's business:
it calls `KeyV` and `PointerV` as the real system does, so everything
above — the key buffer, `*FX` settings, the Wimp — works unchanged.

The key numbering is worth one example, because it shows the shape of
these interfaces. The **End** key on a PC keyboard carries Linux's
`KEY_END`. The table turns that into RISC OS low-level key 53, and the
keyboard layout turns key 53 into character `&8B` — which is **Copy** on
an Archimedes keyboard, and drives the screen editor. Nothing above the
table knows that Linux was involved.

### 5.4 Sound

The sound chain is filled by one callback on a thread of the platform's
own, which runs from start-up so that the queue has a clock whether
anything is making a noise or not. The chain is 16-bit stereo at the
system rate; each filler adds into a buffer that starts at zero, and the
result goes to the device.

On the box that device is ALSA. In the Mac application it is CoreAudio,
and the only difference is the file that implements the handful of
`ros_audio_*` functions. Everything above — SharedSound, the Sound
Scheduler, `SOUND` and `ENVELOPE`, and RISC OS's own 8-bit voices — is
RISC OS's code, translated, and is the same in both.

### 5.5 Filing systems

BOX has no Acorn filing systems. There is no ADFS, no FileCore, no disc
format to understand. FileSwitch is rewritten in C and keeps its usual
interface, and underneath it HostFS presents a Linux directory as a RISC
OS filing system.

That directory may be a folder on the Mac, an ext4 partition on a USB
stick, or an SMB share. RISC OS names are mapped to Linux names, and the
file type is carried in the name as a `,xxx` suffix where the host
filing system cannot hold it otherwise. ResourceFS serves the ROM's own
files out of the arena.

The gain is that the kernel does the work it is good at — caching,
journalling, talking to real controllers — and BOX inherits every
filing system and every device Linux supports.

### 5.6 The network

The network modules are kept as interfaces over Linux rather than as
implementations. The Internet module's socket SWIs become Linux socket
calls; the address is configured by the kernel's own DHCP; the Resolver
asks the system resolver. Above them, AcornSSL is OpenSSL, URL_Fetcher
is libcurl, and the SMB client and server speak SMB2 and SMB3 in place
of the SMB1 that RISC OS's own Access and LanManFS spoke.

A RISC OS program sees the SWIs it expects. The difference is that the
protocols are current ones, maintained by other people.

### 5.7 Time

`OS_ReadMonotonicTime` is the monotonic clock, in centiseconds since the
runtime started. The 50 Hz tick that RISC OS expects — the cursor's
flash, the palette's flash, `MetroGnome` in zero page, null events — is
driven by a timer thread.

### 5.8 Faults

On real hardware a program that reads a bad address takes a data abort,
and the OS turns that into an error the program can handle. BOX does the
same thing with the same parts in a different order.

The kernel delivers the fault as a signal — `SIGSEGV`, `SIGBUS`,
`SIGILL`, `SIGFPE` or `SIGTRAP`. The personality's handler decides what
RISC OS would have made of it and raises that: a data abort at the
faulting address, reported in RISC OS's own words, with the registers as
they were. If the program has an error handler, it is entered. If a SWI
was in progress, the error is returned from the SWI in the ordinary way,
and the task carries on.

Two cases are worth naming because they are not RISC OS faults at all:

* A page of application space that could not be given — the machine's
  memory is used up — arrives as a bus error. RISC OS has no such
  fault, because its slot's pages were taken when the slot grew. BOX
  reports it as the program's data abort, named for what it is.
* A fault in the box's own translated code, outside any SWI, is
  reported in full on the console: the faulting function, the address,
  the registers, and the last few dozen SWIs with their arguments. That
  is a bug in BOX, and the report is meant to be pasted into one.

### 5.9 Tasks as threads

Each RISC OS task is a POSIX thread of `/init`, with the task's own
slot, entry point and stack. Section 6 describes how only one of them
runs at a time. Threads are named, so that a machine that is busy can be
examined with the usual Linux tools and the answer means something.

### 5.10 Parallel work

RISC OS tasks cannot run in parallel: the system they call is
co-operative, and making them parallel would change what programs mean.
Everything that *is* parallel in BOX is therefore Linux work that never
runs RISC OS code:

* the display converter
* the sound filler
* background helpers for slow transfers
* the **Worker** pool, which is BOX's own addition. A program hands a
  Worker a job and a kernel — a piece of compiled code with no access
  to RISC OS — and the pool runs it on the other processors. `!Mandel`
  draws about 6.7 times faster on seven workers.

### 5.11 Going the other way: UnixBridge

Everything above describes RISC OS reaching Linux through the platform
layer. UnixBridge is the opposite direction, and it exists for ported
POSIX software such as NetSurf.

A POSIX application in BOX is a RISC OS task whose C library is musl,
and whose system calls arrive as `SWI Unix_Syscall`. Each call is then
passed to Linux, converted, answered from the task's own state, or
refused. Passing is cheap for the same reason as everywhere else: a
pointer argument is an arena address, which is already a host address,
so a buffer needs no copying.

### 5.12 The ARM container

Some software exists only as ARM binaries. BOX runs those in an ARM
container: user-mode emulation of the ARM processor, with every SWI
answered by BOX's native code rather than emulated. The emulated program
and the native OS share the arena, so again nothing is marshalled across
the boundary. Only the program's own instructions are emulated, which is
why an emulated application still feels quick.

### 5.13 Starting and stopping

The kernel command line carries the box's options, which are read
directly (`rosgd.display=`, `rosgd.boot`, `rosgd.switrace` and so on).

`OS_Reset` — and the desktop's Shutdown, then Restart — is Linux's
`reboot()`. A virtual machine answers it by starting the box again in
the same process, as a real machine restarts. Powering off ends the run
and reports a status on the console.

---

## 6. Tasks

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

The lock also stands in for "interrupts off". Where RISC OS would
disable interrupts to keep a structure still, BOX holds the lock.

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

## 7. SWIs

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

## 8. Summary of design rules

1. A RISC OS address is a host address. Data is never converted or copied
   between RISC OS and the host.
2. The kernel looks after the hardware and the personality looks after
   RISC OS.
3. There is one program. There are no shared libraries, no separate OS
   process, and no forwarding of calls between threads.
4. One processor and one lock. Only one task runs at a time, on its own
   thread. Work that is not RISC OS's may use the other processors.
5. A SWI is a function call, with no trap.
6. Where Linux already does a job well — filing systems, drivers,
   network protocols — RISC OS's module keeps its interface and Linux
   does the work.

Most of these rules exist to avoid work. Rule 1 saves a copy at every
interface, rules 3 and 4 remove a thread or process boundary from every
call, and rule 5 removes a trap from the most heavily used path in RISC
OS.
