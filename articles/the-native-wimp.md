# The Native Wimp

*3 October 2026*

## Introduction

The Window Manager, or Wimp, is the centre of the RISC OS desktop. It
owns the screen, draws every window and icon, delivers every click and
key, and switches between tasks. Since 2 October 2026 BOX's Wimp has
been a new one, written in C from a specification. It replaces the
translation of RISC OS 5's own Wimp that BOX ran before.

This article describes:

* why the Wimp was rewritten, and how
* how it compares with the translation, in speed and accuracy
* why BOX keeps RISC OS's task memory layout, with each task's slot at
  `&8000`
* why BOX's Wimp slots are dynamic, and the memory needs of 64-bit host
  code that make that necessary.

---

## 1. Why rewrite the Wimp

The first Wimp in BOX was RISC OS 5.88's Window Manager, translated from
its ObjAsm by ROSASM at tier 1 (see *Tiers of Translation*). It worked,
and it ran the desktop until 2 October. But the Wimp is where a
translation costs the most, and it is the module that every desktop
program calls most often.

The translation was 103,268 lines of C from 64K lines of ObjAsm. Its
x86-64 code was five to six times the size of the ARM original. It kept
the assembler's structure throughout:

* registers spilled and reloaded around every call and SWI, about
  29,000 times;
* flags stored at about 2,000 compares;
* 1,258 `setjmp` resume points and 1,976 checked returns, to reproduce
  the assembler's non-local exits, such as leaving a menu selection or
  returning from deep inside `Wimp_Poll`;
* the Wimp's calls to its own SWIs going through the general SWI
  dispatcher.

None of that can be tuned away. It is the cost of keeping the
assembler's shape. And the code could not easily be changed, so the
Wimp could not grow with the rest of BOX.

---

## 2. How it was written

The native Wimp was written to a specification: thirteen chapters,
taken from the ObjAsm source, the PRMs and RISC OS 5.30's behaviour on
real hardware. They cover tasks and `Wimp_Poll`, windows, icons, input,
messages, the iconbar, the caret, menus, error boxes, and how the
furniture and icons are drawn, pixel by pixel. Where the source and the
PRM disagreed, the behaviour of 5.30 on the farm decided.

It is about 17,800 lines of C, a sixth of the translation, in modules a
person can read: tasks, messages, windows, redraw, rectangle lists,
icons, menus, the caret, input, the command window, the clipboard and
so on. It is a native module, the same C on both boxes, and keeps its
own state in ordinary C structures rather than a workspace laid out in
the RMA. Client blocks (window definitions, icon blocks, the poll block,
templates) keep their RISC OS layouts exactly.

Some of RISC OS's undocumented behaviour had to be kept on purpose
because other code relies on it: the Filer calling into the Wimp's
sprite routines through `Wimp_Extend`, the Toolbox relying on a fake
null event after `Wimp_StartTask`, and the order in which rectangles
come back from a redraw.

### Testing

* **Against RISC OS 5.30.** The Wimp test suite (34 cases: tasks,
  messages, keys, mouse selection and drags, visual output, mode changes
  and more) runs each case on the farm's real RISC OS 5.30 and in BOX,
  and the logs must match. Mouse cases are driven on the farm through
  its emulated USB tablet.
* **Against the translated Wimp.** Both Wimps are in the ROM, chosen at
  boot (`rosgd.wimp=translated` brings back the old one), so a case the
  farm cannot show can be judged against the translation in the box.
  Screens are compared pixel for pixel.
* **The desktop probes** drive real applications through the Filer, as
  a user would.

### What it costs and gains

Timed on the Apple silicon box, median per operation:

| Operation | Translated | Native |
| --- | --- | --- |
| `Wimp_Poll` | 3.0 µs | 1.3 µs |
| A message | 3.3 µs | 1.8 µs |
| A redraw | 9,517 µs | 6,308 µs |
| Two task switches | 29.4 µs | 8.0 µs |
| Typing into an icon | 211 µs | 114 µs |

The price is accuracy. A translation is as exact as its translator; a
rewrite is only as good as its specification. Testing found places where
the specification was wrong (a child task that printed nothing still has
the mode set again when it starts, which the specification had said it
did not). At the time of writing, one fault, with StrongED's modes,
appears only under the native Wimp.

---

## 3. The task memory layout

On RISC OS every Wimp task has its own **application slot**, always at
the same address, `&8000`. Only the running task's slot is visible
there. When the Wimp switches tasks, it pages one slot out and the next
one in.

BOX keeps this exactly. It would have been easy to do something more
Unix-like (a separate process for each task, or one shared address
space with programs placed wherever they fit), but RISC OS programs
depend on the layout:

* **Programs are built for `&8000`.** An Absolute file is loaded and
  entered there. BOX's own C applications are linked to run there too.
* **BASIC** puts its program at `PAGE`, its variables above it, and its
  stack below `HIMEM`, all within the slot.
* **The slot grows from its top.** `flex`, used by Edit, Draw, Paint and
  many other C programs, grows its heap by asking `Wimp_SlotSize` for a
  bigger slot. `*WimpSlot` in a `!Run` file sets the size before the
  program starts. The Task Manager lets the user drag a slot's size.
* **Tasks cannot see each other's memory.** Data passes between them by
  `Wimp_TransferBlock` and messages. Code that runs whatever task is
  paged in (drag routines, pollwords, filters) must be in the RMA.

In BOX each slot is a Linux memory object. Paging a task in is mapping
its object at `&8000`. This happens during `Wimp_Poll`, on the thread of
the task giving up the processor, exactly where RISC OS switches. Where
the Wimp must read another task's memory (a menu block or an indirected
icon's text, which belong to their owners), it maps that task's slot in
while it reads, as RISC OS does.

---

## 4. Dynamic Wimp slots

On RISC OS, slots are sized by hand. The author of a program chooses a
figure, puts it in `!Run` as `*WimpSlot -min 256K -max 256K`, and the
Wimp takes that much memory from the free pool. If the figure is too
small, the program fails with "No room"; if it is too large, the memory
is wasted while the program runs.

### The memory needs of 64-bit code

BOX runs 64-bit host code (A64 or x86-64), not ARM code, and the same
program needs more memory:

* **Code is larger.** ARM code is dense: one instruction can load,
  shift, test a condition and update a register at once. The translated
  Wimp's x86-64 code was five to six times the size of its ARM image;
  BBC BASIC's A64 code is about seven times its ARM image.
* **Stacks are deeper.** Registers and stack slots are 8 bytes wide,
  even when pointers are 4.
* **Programs carry more.** A trivial ROSBAS program is a 147 KB image,
  and needs a slot of at least 1 MB once its stack is counted.

So the slot sizes in existing `!Run` files, which were chosen for ARM
code and often cut fine, cannot be trusted. Nor can the author of a
port easily know the right new figure.

### What BOX does

BOX does not make anyone guess. Since 2 October 2026:

* **Application space is 1.5 GB**, from `&8000` to `&60000000`.
* **Every task is given the whole 1.5 GB** unless it asks for less.
  `*WimpSlot -min … -max …` and a program's own `Wimp_SlotSize` calls are
  still honoured, so programs that manage their own memory, as `flex`
  does, work as before.
* **Slots are lazy.** A slot is a file in a memory-backed slot pool, and
  costs only the pages its program actually touches. A task with a
  1.5 GB slot that uses 300 KB costs 300 KB.
* **The figures are real.** The free pool reported to programs is the
  box's real free memory, and the Task Manager shows the memory each
  task is using, not the size of its slot, updated as it changes.
* **Running out is an error, not a crash.** If the box truly runs out
  of memory, the program that asked gets a RISC OS error ("abort on data
  transfer"), and the rest of the desktop carries on.

The RMA and the system heap keep fixed sizes, as on RISC OS, because
they are shared by every task.

The result is that a program ported from ARM does not need its slot
size worked out again, and the extra size of 64-bit code costs real
memory only where it is used.
