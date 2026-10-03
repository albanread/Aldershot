# BOX Architecture

*An article in the BOX series. 3 October 2026.*

BOX is RISC OS, on a Linux kernel, translated to C. This article describes
the architecture as it is built: the division of work between the kernel
and the RISC OS personality, the arena in which the two meet, the way a
RISC OS task is a thread, and the way a SWI is a call. It describes what
runs; nothing in it is a plan.

Notation follows RISC OS convention. Numbers prefixed with an ampersand are
hexadecimal: `&8000`. Registers are R0 - R15; SWIs are written `OS_WriteC`;
star commands are written `*Run`.

---

## 1. The system in outline

There are three parts.

```
        0                                   4 GB
        +-------------------------------------+
        |              the arena              |   a RISC OS address
        |  the application slot, the RMA, the |   is a host address;
        |  screen, the ROM areas, zero page   |   nothing is translated
        +-------------------------------------+
       raised by                    served by
       the personality              the kernel
       RISC OS 5 translated         pages, memfds, threads,
       to C; one program;           devices, filing systems
       tasks as threads; SWIs
       as calls
```

*Figure 1. The kernel provides what hardware provides; the personality
provides RISC OS; the arena is the memory both of them see.*

The kernel provides what a kernel provides: pages of memory, threads,
device services, filing systems. The personality is RISC OS 5, translated
from the sources published by RISC OS Open and compiled as native code.
The personality owns no device and no filing system of its own; when RISC
OS reads a disc or lights a pixel, the work is done by the kernel, called
by the personality, at the moment RISC OS asks for it.

The two never translate data between them. The arena is that agreement,
and section 4 describes it.

---

## 2. The kernel

The kernel is Linux. It is not modified. The personality is the first
process, `/init`, and everything the box does is done by `/init` and its
threads.

`/init` is a static PIE. This is a rule of the design, not a convenience:
a static binary that is not position-independent is loaded by the kernel
at 16 MB, which is inside the arena, and the arena's reservation would
fail. A PIE is loaded far above 4 GB, where no RISC OS address can reach
it. There are no shared objects anywhere in the box; the personality and
every module of RISC OS it contains are one program.

At boot, `/init` mounts `devtmpfs`, `proc` and `sysfs`, and sets
`vm.mmap_min_addr` to `&4000`. The kernel's configuration permits
mappings at `&8000`, where the application slot begins; the personality
lowers the setting once more because ScratchSpace lies at `&4000`, below
application space.

The kernel supplies the devices. The screen is a graphics buffer of the
kernel's making, mapped into the arena at `&A0000000`; storage, input and
sound likewise arrive as services of the kernel, and the personality calls
them. A fault in a program is delivered to the personality, which turns it
into the RISC OS error the program expects; it does not end the box.

### The one-processor rule

Every thread of the personality runs on one CPU, the CPU on which `/init`
began. Only the task holding the baton runs (section 4), so a second CPU
would add nothing but the cost of waking a thread upon it. The cost was
measured under the development virtual machine:

| To wake a thread | Cost |
| --- | --- |
| on the CPU the switcher runs on | about 6 µs |
| on another virtual CPU | about 58 µs |

RISC OS is one processor; its tasks stay on one.

---

## 3. The personality

The personality is RISC OS 5.31, translated to C from the sources of
RISC OS Open and compiled as native code for the host processor, with
32-bit pointers: a pointer of the C language is a RISC OS address, and 32
bits suffice because no RISC OS address exceeds the arena.

The modules of RISC OS - the kernel, FileSwitch, the Wimp, the Font
Manager, BASIC and the rest - are translated with it
and linked into `/init`. A module's code is not loaded; it is already
present. What RISC OS calls the ROM is a region of the arena,
`&FC000000`, in which each module's code and static data are laid out;
RISC OS programs that read the ROM find what they expect.

Code is entered through one contract. An entry into compiled code is a
function

```c
void ros_code(struct ros_cpu *cpu);
```

and `struct ros_cpu` is the register state: R0 - R15 (R13 the stack
pointer, R14 the link register, R15 the program counter, all of them arena
addresses), the flags N, Z, C, V and Q held as separate integers, the
modelled mode and I bit, and a pointer to the task's floating-point
state. A program's registers are one small structure, passed by pointer,
and they are the same structure at every boundary in the box.

---

## 4. The arena

The arena is the low 4 GB of the address space, and it is the whole of a
RISC OS program's world. Its rule is stated once and never varies:

> A RISC OS address is a host address.

Nothing is marshalled. A sprite, a font, a file path, a Wimp message
block: what one side writes at `&1C8C00`, the other reads at `&1C8C00`.
Since a RISC OS address is 32 bits, no RISC OS address can name the
memory of the personality above 4 GB; and a pointer of the personality's
own that is not arena memory is refused - when such a pointer reaches
compiled code, the box stops with a report, for it is a fault in the box,
not in the program.

Before anything else, the personality reserves the span from `&8000` to
4 GB as a single inaccessible mapping. Nothing of the kernel's can then
occupy a RISC OS address. The regions are mapped over the reservation at
their fixed addresses:

| Address | Region | Extent | Contents |
| --- | --- | --- | --- |
| `&00004000` | ScratchSpace | 16 KB | the kernel's public scratch area |
| `&00008000` | Application space | the slot | the current task's program and data |
| `&20000000` | Relocatable module area | to 256 MB | modules' code and workspace |
| `&30000000` | System heap | to 32 MB | the system's own allocations |
| `&32000000` | SVC stack | 1 MB | the stack compiled code runs on |
| `&A0000000` | Screen | the mode | the screen buffer |
| `&FC000000` | ROM image | 48 MB | the compiled modules' areas |
| `&FFFF0000` | Zero page | 32 KB | public kernel workspace, at its offsets |

Dynamic areas are mapped into the reservation as they are created. Zero
page carries the kernel's public words at the offsets programs know:
`MetroGnome`, the centisecond counter, at `+&10C`; `ReturnCode` at
`+&AC4`; the current Wimp task's domain at `+&FF8`; and the rest at
theirs.

Every region is a memfd - a memory object of the kernel's, with a name
and a file descriptor - not private memory. This has three consequences.
A region may be mapped again at the same address, which is how a task's
slot is taken over by another task when the Wimp switches, and how the
same regions will be mapped into further processes should the design ever
ask for them. A slot grows by extending its memfd, as RISC OS's module
area grows by extension. And the application slot and the RMA are
executable - application code and module code run from them, as RISC OS's
do - so their memfds are created for execution and the kernel is asked
for that permission once, at boot, and not per program.

---

## 5. Tasks as threads

A RISC OS task is a thread of `/init`. A task is created with its slot,
an entry and an argument; the entry is a function of the personality's
which runs the task's program until the task ends. There is one thread
per task, and no more.

Two rules govern them: the personality lock and the baton.

**The personality lock** is a single mutex. A task's thread holds it
while it runs and releases it to wait. Module code runs holding the lock,
because module code runs in the context of whoever called it: when a task
calls a SWI, the handler runs on the task's own thread, holding the lock
the task already holds. Nothing is forwarded to another thread and no
other thread may touch the personality's state; there is nothing to race
with.

**The baton** is the desktop's single right to run. It is a flag per task
and one mutex. When the Wimp decides to switch tasks - at `Wimp_Poll`,
as RISC OS always has - the switch runs on the thread of the task giving
the baton away, holding the personality lock. That thread saves its own
task's current program and puts the next task's in place: the slot
mappings, the personality's pointers, zero page. When the next task's
thread wakes, everything is already as the task left it. Then the giving
task releases the lock and waits for its own flag.

A task is therefore paged out exactly when RISC OS would page it out, and
blocked exactly where RISC OS would block: inside the call. A SWI that
waits - a poll idle, a sleep, a slow transfer - releases the lock around
its wait; the thread sleeps inside the SWI, background work runs, and the
call returns to the task that made it, on the thread that made it.

The personality counts SWIs in and out of each other. The way out of the
outermost SWI of a task is a safe point: transient callbacks owed to the
task are delivered there, and nowhere else. A program that computes for a
long time without calling RISC OS sees its callbacks late; this is the
one departure from interrupt-time delivery, and it is a bounded one.

---

## 6. The SWI path

A SWI in BOX is a function call. There is no instruction to trap and no
trap to take: translated code and C applications call the dispatcher as a
function of the C language,

```c
void ros_swi(struct ros_cpu *s, uint32_t number);
```

with the register state in `s` and the SWI's number, X bit included, in
`number`. The call is made on the task's own thread, under the
personality lock. The cost is the cost of a function call, about 0.1 µs,
and there is nothing between the program and its SWI.

The number decides the handler:

| Numbers | Handler |
| --- | --- |
| `&00` - `&FF` | the kernel's own SWIs, dispatched to their translated implementations |
| `&100` - `&1FF` | `OS_WriteI`: one SWI per character, the character in the number |
| all others | a module's chunk of 64, dispatched to that module's handler |

A module's handler is entered as RISC OS's own dispatcher enters it: R11
holds the offset within the chunk, R12 the module's private word, and
R10 - R12 are preserved for the caller. `OS_CallASWI` and
`OS_CallASWIR12` are honoured: the SWI whose number stands in R10 or R12
is dispatched in the place of the call, and its own X bit decides its
error behaviour.

The X bit is bit 31 of the number, as in RISC OS. An error in an X-form
call is returned: V is set in the register state and R0 holds the address
of the error block, which is arena memory, so the caller reads it as an
ordinary pointer. An error in a call without the X bit is raised: the
personality delivers it to the task's error handler, with the registers,
mode and stack that RISC OS 5.31's kernel enters an error handler with.
The error handler of a translated task is translated code; the delivery
is the same nested entry that any handler receives.

---

## 7. The rules of the design

Stated together, the rules that make the box what it is:

1. A RISC OS address is a host address. Nothing is marshalled, anywhere,
   ever.
2. The kernel owns the hardware; the personality owns RISC OS. Neither
   does the other's work.
3. One program. No shared objects, no second process for the OS, no
   forwarding between them.
4. One processor. One lock. One task runs at a time, and it runs on its
   own thread, in its own call.
5. A SWI is a call. There is no trap on the path between a program and
   the OS it is calling.

Each rule removes work. The first removes a copy and a translation from
every interface; the third and the fourth remove a boundary from every
call; the fifth removes a trap from the most frequently taken path in
RISC OS.
