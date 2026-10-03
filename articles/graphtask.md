# GraphTask

*3 October 2026*

## Introduction

A graphical BBC BASIC program takes the whole screen. It changes the
mode, draws where it likes, sets the palette, and the desktop is gone
until it ends.

On RISC OS there was a utility, GraphTask, that got round this. It
opened a desktop window showing a graphical screen in one of BASIC's
modes, and ran a command line in it. What the program drew went into
the window, and the rest of the desktop carried on around it. The
graphical task window was always my favourite way to run BASIC
programs: you could write a program in one window, run it in another,
and see both at once.

BOX has its own GraphTask, built into the system rather than added
from outside. This article describes the original's trick, why it was
fragile, and how BOX does it.

---

## 1. What the original did

The original GraphTask was closed source, and it worked by
interception. It ran the program as a child, as a task window does,
and around each of the child's time slices it switched the VDU's output
to a sprite (`OS_SpriteOp 60`), then back to the screen for the rest of
the desktop. Everything else was caught on the way past: vectors
claimed to spot mode changes, and patches where RISC OS offered no
hook.

It worked often enough to be loved. It broke on anything it had not
anticipated: a program that read the screen's address and wrote to it,
changed the palette, or ran on past the end of its time slice.

---

## 2. What BOX does instead

BOX owns its VDU drivers, its Window Manager and TaskWindow: all three
are BOX's own C. So instead of faking the redirection from outside,
BOX makes it a property of a task. A task can have a **virtual
display**: a screen of its own, in a mode of its own, which nothing
else sees.

While the task's own code runs, the VDU draws into the virtual display,
and every call that reads or changes the screen answers for it. Every
other task, and the desktop itself, carries on with the real display.

### The VDU's state is one structure

All of the VDU's state (the mode variables, the screen address, the
cursors, the text and graphics windows, the colours, the patterns, the
palette, the soft font, where output goes) is one C structure. Swapping
the whole structure swaps the VDU. A handful of things kept elsewhere,
such as the changed box and the OR and EOR tables, are swapped with it.

A saved copy of that state is a **VDU context**. There is one for the
real display and one for each virtual display, and exactly one is live
at any moment. Swapping copies about 6 KB.

### When each context is live

The program's own drawing, and the modules it calls to draw (Draw,
ColourTrans, the Font Manager, SpriteExtend), use its virtual display.
Some things must always use the real display, whichever task is
running:

* **background work**: the ticker, the pointer, the hourglass, sound;
* **callbacks**, including the task window's own time slice;
* **every Window Manager call**, since the Wimp draws window borders,
  menus and error boxes, and switches tasks, inside its own calls.

BOX swaps to the right context when the Wimp switches tasks, on the way
in and out of each of those, and at each call the program makes. That
last check also puts things right if the program leaves a section
abruptly, through an error or `OS_Exit`.

The graphics driver itself is never told about a virtual display. A
program's `MODE`, its palette changes and its screen-bank flips stay in
its own context, and the real screen never hears of them.

### The display is a sprite

A virtual display's memory is a dynamic area holding two screen banks.
Each bank is a sprite area containing one sprite, `screen`, in the
display's mode, and the sprite's pixels *are* the screen memory. So:

* a program that reads the screen's address (`ScreenStart`) and writes
  to it writes into the sprite;
* the two banks work as on RISC OS (`OS_Byte 112` and `113`), so
  double-buffered animation is smooth;
* showing the display is plotting a sprite, scaled, with a colour table;
* "save screen" is saving the sprite.

### The mouse and the keys

The mouse (`MOUSE`, `OS_Mouse`) and key scanning (`INKEY` with a
negative number) answer for the program only while its window has the
input focus, and the mouse's position is given in the virtual display's
coordinates. A program that draws with the mouse works inside its
window.

---

## 3. Using it

`!GraphTask` is in the ROM's applications (`Resources:$.Apps`). Its icon
on the icon bar opens a window: a mode 28 screen running the `*` prompt,
where you can type `*BASIC`, `CHAIN` a program, or run any command.
Each window has its own display, and its own program running in it.

* **Keys** typed in the window go to the program, as in a task window.
* **The window's menu** has Scale (1×, 2×, or fit to the window), Mode
  (12, 15, 20, 21, 27, 28 or 31, or one typed in), Freeze, Save screen,
  Reset and Kill.
* **A mode change** by the program resizes and retitles the window.
* **The window refreshes** about fifty times a second, redrawing only the
  part that has changed.

From the command line, `*GraphTask <command>` runs a command in a new
window, in the running GraphTask if there is one. **StrongED** uses this:
its Run, for a BASIC program that draws, opens the program in a
GraphTask window instead of giving it the whole screen.

The disc's `Examples.GraphTask` folder has three programs to try: Spin,
Bounce, which animates with two screen banks, and Sketch, which draws
with the mouse.

---

## 4. Under the window

GraphTask is built from three pieces, and the app itself is small:

* **VDisplay**, a module of BOX's own (SWIs from `&C01C0`), creates and
  destroys virtual displays, attaches one to a task, reports its mode,
  its sprite and what has changed, and is told where the window is and
  whether it has the focus.
* **TaskWindow** takes one new option, `-vdisplay`. A task window's
  child with a virtual display sends its output to the VDU, and so into
  the display, instead of to its parent as text. Input is unchanged.
* **`!GraphTask`** creates a display, starts a task window's child on
  it, and shows the display's sprite in its window.

None of this needed the parent to know anything special: a BASIC Wimp
task of twenty lines can do what GraphTask does, without the window.

---

## 5. What it does not do

* **A loop that makes no system calls** holds the desktop until it ends,
  as it would in any task window. A task window's program gives way only
  when it calls RISC OS, and a graphical program does that all the time:
  every `PLOT`, every character, every wait for the screen. A pure
  calculation loop does not.
* **Pointer shapes** a program defines for itself are not shown yet.
* **Teletext** (mode 7) flashing is not shown yet.

---

## 6. Tested

BOX's self-test has 18 checks for virtual displays, among them:

* `MODE` changes the virtual display, not the real screen;
* drawing lands in its pixels and not on the real screen;
* the mode and VDU variables read back the virtual display's;
* two virtual displays are independent;
* output to a sprite works inside one;
* background work still uses the real display;
* the mouse and `INKEY` follow the focus.

A program drawing circles, lines and text in mode 28 inside a task
window's virtual display was saved as a sprite and inspected, and the
Paint desktop test showed the real display unharmed.
