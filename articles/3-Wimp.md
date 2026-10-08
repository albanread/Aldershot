# The Native Wimp

*8 October 2026*

## Introduction

The Window Manager, or Wimp, is the centre of the RISC OS desktop. It
owns the screen, draws every window and icon, delivers every click and
key, and switches between tasks. Since 2 October 2026 BOX's Wimp has
been a new one, written in C from a specification. It replaced the
translation of RISC OS 5's own Wimp that BOX ran before.

This article describes why it was rewritten and how; what the rewrite
cost and gained; what a Wimp that can be changed has since made
possible; and why BOX keeps RISC OS's task memory layout, with every
task's slot at `&8000`, while making the slots themselves dynamic.

---

## 1. Why rewrite the Wimp

The first Wimp in BOX was RISC OS 5's own Window Manager, translated
from its ObjAsm by ROSASM at tier 1 (see *Tiers of Translation*). It
worked, and it ran the desktop until 2 October. But the Wimp is where a
translation costs the most, and it is the module every desktop program
calls most often.

The translation was 103,268 lines of C from 64K lines of ObjAsm, and its
host code was five to six times the size of the ARM original. It kept
the assembler's shape throughout:

* registers spilled and reloaded around every call and SWI, about
  29,000 times;
* flags stored at about 2,000 compares;
* 1,258 `setjmp` resume points and 1,976 checked returns, to reproduce
  the assembler's non-local exits — leaving a menu selection, or
  returning from deep inside `Wimp_Poll`;
* the Wimp's calls to its own SWIs going round through the general SWI
  dispatcher.

None of that can be tuned away. It is what it costs to keep the
assembler's structure.

The second reason mattered more than the first, and it is easy to
undersell. **The translation could not be changed.** A translated module
is correct, and it is a reasonable thing to run, but nobody can sensibly
add a feature to 103,268 lines of register-shaped C. The Wimp could not
grow with the rest of BOX. Section 5 is what happened once it could.

---

## 2. How it was written

The native Wimp is a tier 2 rewrite: written by hand from a
specification rather than from the code. The specification is thirteen
chapters, taken from the ObjAsm source, the PRMs and RISC OS 5.30's
behaviour on real hardware. They cover tasks and `Wimp_Poll`, windows,
icons, input, messages, the icon bar, the caret, menus, error boxes, and
how the furniture and icons are drawn, pixel by pixel. Where the source
and the PRM disagreed, 5.30's behaviour on the farm decided.

It is now about 19,100 lines of C — under a fifth of the translation —
in parts a person can read: tasks, messages, windows, redraw, rectangle
lists, icons, menus, the caret, input, the command window, the clipboard
and the rest. It is the same C on both boxes, and it keeps its own state
in ordinary C structures rather than in a workspace laid out in the RMA.
Blocks that clients can see — window definitions, icon blocks, the poll
block, templates — keep their RISC OS layouts exactly, because programs
read and write them directly.

Some of RISC OS's undocumented behaviour had to be kept on purpose,
because real software depends on it: the Filer calling into the Wimp's
sprite routines through `Wimp_Extend`, the Toolbox relying on a fake
null event after `Wimp_StartTask`, and the order in which rectangles
come back from a redraw.

### Testing

* **Against RISC OS 5.30.** The Wimp test suite — now 39 cases, covering
  tasks, messages, keys, mouse selection and drags, visual output, mode
  changes and more — runs each case on the farm's real RISC OS 5.30 and
  in BOX, and the logs must match. Mouse cases are driven on the farm
  through its emulated tablet.
* **Against the translated Wimp.** Both Wimps are still in the ROM and
  are chosen at boot: `rosgd.wimp=translated` brings back the old one.
  So a case the farm cannot show can still be judged against the
  translation in the box, screen against screen, pixel for pixel. This
  is the tier 0 idea from *Tiers of Translation* applied to a module:
  keep the exact thing you replaced, and use it as the oracle.
* **The desktop probes** drive real applications through the Filer, as a
  user would.

### What it cost and gained

Timed on the Apple silicon box at the time of the rewrite, median per
operation:

| Operation | Translated | Native |
| --- | --- | --- |
| `Wimp_Poll` | 3.0 µs | 1.3 µs |
| A message | 3.3 µs | 1.8 µs |
| A redraw | 9,517 µs | 6,308 µs |
| Two task switches | 29.4 µs | 8.0 µs |
| Typing into an icon | 211 µs | 114 µs |

The price is accuracy, and it is the price every tier 2 rewrite pays: a
translation is as exact as its translator, but a rewrite is only as good
as its specification. Testing duly found places where the specification
was wrong — it said a child task that had printed nothing would not have
its mode set again, and the farm showed that it does.

One fault from the changeover is still open at the time of writing: in
the StrongED modes probe, a drag from a Filer window to the icon bar
misses, where the translated Wimp passes. It is logged as issue #108.
That is the honest state of it; a rewrite does not become exact by being
declared finished.

---

## 3. What a Wimp you can change makes possible

This is the part that does not show up in a timing table.

### Surface windows

A Wimp window can now be **bound to a surface** — a sprite, or a virtual
display — which the Wimp then composites into the window when it
redraws. The program that owns the surface draws into it whenever it
likes and tells the Wimp what changed; the Wimp does the rest, including
scaling.

The interface is four calls on `Wimp_Extend`: `SurfaceBind`,
`SurfaceUnbind`, `SurfaceChanged` and `SurfaceInfo`.

Two things in BOX use it, and they are not alike:

* **GraphTask** puts a BBC BASIC program's screen in a desktop window.
  The original did this by interception and trickery. Here the program
  draws into a virtual display of its own, and the window shows it.
* **The WPE edition's Linux programs.** A Wayland window — a browser,
  say — is bound to a surface, so **a Linux program's window is a Wimp
  window**: it has RISC OS furniture, it is dragged and stacked with the
  others, and the Wimp's own scroll bars and size icon drive it.

Neither was possible before. Both are a handful of calls into a Wimp
whose redraw path a person can read.

### A week's worth of ordinary fixes

The rest of it is less glamorous and just as much the point. Since the
rewrite the Wimp has taken fixes for a held window drag flooding its
owner with `Open_Window_Request`, null events paced to tasks that poll
busily, `Wimp_TextOp` overwriting its own text when it changed colour,
`SendMessage` checking a block size it should not, `Wimp_SetExtent` not
redrawing the scroll bars, the pointer's colours, Wimp tasks started
from SSH sessions, and an icon's sprite size taken from mode variables
that could not be read.

Each is a few lines. Under the translation each would have been a
research project, if it had been attempted at all.

### And one deliberate departure

Changing a narrower screen mode used to lose the icons at the
right-hand end of the icon bar — Display's, Configure's, whatever a task
had put there — and nothing brought them back. They were not lost: they
were sitting where the wider screen had put them, out past the new edge,
and the bar does not pan.

That is not a translation bug. RISC OS 5.30's own specification says in
as many words that the right-hand group is placed from the *old*
extent, so the real system does the same thing. BOX now refits the bar
to the narrower screen instead, on a plain principle:

> If there is room on the icon bar to show all the icons, show them all.

It is worth being clear about what that means. A rewrite gives you the
power to fix things the original got wrong, and then it makes you decide
whether to use it. BOX's answer here is that a user looking for a
missing icon is not well served by faithfulness. Such departures are
made deliberately, one at a time, and written down — not allowed to
creep in.

---

## 4. The task memory layout

On RISC OS every Wimp task has its own **application slot**, always at
the same address, `&8000`. Only the running task's slot is visible
there; when the Wimp switches tasks it pages one out and the next in.

BOX keeps this exactly. Something more Unix-like would have been easier
— a process per task, or one address space with programs placed wherever
they fit — but RISC OS programs depend on the layout:

* **Programs are built for `&8000`.** An Absolute file is loaded and
  entered there, and BOX's own C applications are linked to run there
  too.
* **BASIC** puts its program at `PAGE`, its variables above it and its
  stack below `HIMEM`, all inside the slot.
* **The slot grows from its top.** `flex`, used by Edit, Draw, Paint and
  many other C programs, grows its heap by asking `Wimp_SlotSize` for a
  bigger slot. `*WimpSlot` in a `!Run` file sets the size beforehand, and
  the Task Manager lets the user drag it.
* **Tasks cannot see each other's memory.** Data passes between them by
  `Wimp_TransferBlock` and by messages. Code that must run whatever task
  is paged in — drag routines, pollwords, filters — has to live in the
  RMA.

In BOX each slot is a Linux memory object, and paging a task in is
mapping its object at `&8000`. That happens inside `Wimp_Poll`, on the
thread of the task giving up the processor, exactly where RISC OS
switches. Where the Wimp must read another task's memory — a menu block,
or an indirected icon's text, which belong to their owners — it maps
that task's slot in while it reads, as RISC OS does.

---

## 5. Dynamic Wimp slots

On RISC OS, slots are sized by hand. The author picks a figure, puts it
in `!Run` as `*WimpSlot -min 256K -max 256K`, and the Wimp takes that
much from the free pool. Too small and the program fails with "No room";
too large and the memory is wasted while it runs.

### Why 64-bit code forces the issue

BOX runs 64-bit host code, not ARM code, and the same program needs more
memory:

* **Code is larger.** ARM code is dense — one instruction can load,
  shift, test a condition and update a register at once. The translated
  Wimp's host code was five to six times the size of its ARM image, and
  BBC BASIC's is about seven times.
* **Stacks are deeper.** Registers and stack slots are 8 bytes wide even
  where pointers are 4.
* **Programs carry more.** A trivial compiled BASIC program is a 147 KB
  image, and wants a slot of at least 1 MB once its stack is counted.

So the slot sizes in existing `!Run` files — chosen for ARM code, often
cut fine — cannot be trusted, and the author of a port cannot easily
work out the right new figure either.

### What BOX does

BOX does not make anyone guess:

* **Application space is 1.5 GB**, from `&8000` to `&60000000`.
* **Every task is given all of it** unless it asks for less.
  `*WimpSlot` and a program's own `Wimp_SlotSize` calls are still
  honoured, so programs that manage their own memory, as `flex` does,
  work exactly as before.
* **Slots are lazy.** A slot costs only the pages its program actually
  touches. A task with a 1.5 GB slot that uses 300 KB costs 300 KB.
* **The figures are real.** The free pool reported to programs is the
  box's real free memory, and the Task Manager shows what each task is
  using rather than the size of its slot.
* **Running out is an error, not a crash.** If the box truly runs out,
  the program that asked gets a RISC OS error — "abort on data
  transfer" — and the rest of the desktop carries on.

The RMA and the system heap keep fixed sizes, as on RISC OS, because
every task shares them.

The result is that a program ported from ARM does not need its slot size
worked out again, and the extra size of 64-bit code costs real memory
only where it is actually used.
