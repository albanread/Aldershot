# The Compositing Wimp

*16 October 2026*

## Introduction

Until October 2026 the BOX desktop worked as every RISC OS desktop has
worked since 1987. There was one screen, held in memory. Every task drew
straight into it, and the Window Manager kept order by telling each task
which rectangles were its own to redraw. On the PC builds the desktop
now works differently. Every window has a picture of its own, kept in
memory of its own. A separate program puts these pictures together on
the graphics card, with shadows and rounded corners, over a backdrop
the graphics card draws itself. The screen in memory is still there,
but nothing shows it.

This article describes:

* why the single screen was given up
* the layers the desktop is now built from, and the services each uses
* a window's backing, the memory a window is drawn into
* block v2, the list that tells the graphics card what to show
* the presenter, the program that composites the desktop
* the path from a task's drawing to the screen, step by step
* the decision to run everything under the Wimp, and what that means for
  programs that were never Wimp tasks
* vector icons, and how the old bitmap themes and the new painted ones
  live together.

The code is mainly `modules/wimp` (the Wimp's side) and `ports/present`
(the presenter).

---

## 1. Why the single screen went

On a single screen, moving a window means two things. The Wimp copies
what it can of the window to its new place. Then every task whose
window was uncovered must redraw what has come into view. Until a task
has done that, the screen shows whatever was there before, which on
BOX was often black. A small window moved quickly showed black edges.
A window dragged over the browser left a trail. The caret, drawn by
exclusive-OR straight into the screen, could be lost when the text
under it was redrawn.

Each of these faults could be mended on its own, and many were. But
they all had the same cause. A window's picture lived only on the
screen, and anything that covered it destroyed it. We decided to stop
mending the symptoms and remove the cause. Each window now keeps its
own picture, and what is on the screen is made from those pictures
afresh each time something changes. Nothing that one window does can
damage another.

The graphics card does the putting together. A PC graphics card does
little else all day on other systems, and it does it quickly. The
processor is left to RISC OS.

---

## 2. The layers

The screen is built from three layers, from the bottom:

| Layer | What it is | Drawn by |
| --- | --- | --- |
| 0 | The backdrop: a picture, a tile, or a shader scene that moves | The presenter, on the graphics card |
| 1 | The screen in memory, RISC OS's framebuffer | Nothing: it is kept mapped but not shown |
| 2 | The surfaces: every window, menu, icon and Linux window | The Wimp and the tasks, each into its own memory |

Layer 1 is kept because programs may still look at screen memory and
expect it to be there. It is mapped at `&B4000000` as before, and a
program that writes to it does no harm. But it is not shown. Programs
that draw by writing screen memory directly are not supported on the
composited desktop. Section 8 says what happens to them instead.

A surface is anything the presenter draws as one picture. These are:

* **Windows.** Every window except the Wimp's own back window has a
  backing (section 3). This includes the Pinboard's backdrop window,
  whose icons sit on the presenter's backdrop.
* **Child windows**, such as toolbars and panes. They are surfaces
  nested inside their parent's: a toolbar is a surface that contains
  other surfaces.
* **Menus and the icon bar.** They are windows to the Wimp, and so they
  are surfaces like any other.
* **Error boxes.** Wimp_ReportError's box is published as a surface
  inside its own modal loop, so it is seen even though no task polls.
* **The drag overlay.** The dashed box of a drag, and a sprite dragged
  by DragASprite, are drawn into a surface of their own over everything
  else.
* **Icon surfaces.** The clock's face and the machine monitor's graph
  are drawn by the graphics card itself. Each asks for its own rate of
  update (the clock thirty times a second), and nothing is copied for
  them.
* **Pages.** A Linux program can hand the presenter a shared-memory
  picture with a list of the rows it changed.
* **Linux windows.** The browser's windows, drawn by WebKit and passed on
  by the Wayland compositor, sit in a Wimp window's work area.

The pointer is drawn by the presenter last of all, as a small texture.

---

## 3. A window's backing

A backing is the memory a window is drawn into. It is a Linux memfd
holding a RISC OS sprite area with one sprite: 32 bits a pixel, with an
alpha channel, the size of the window's outline. It holds the title
bar, the scroll bars and the frame as well as the work area. The memfd
is also published as `/dev/rosgd-backing-<n>`, so the presenter can map
the same pages and take them as a texture without a copy.

Nothing in RISC OS needed to change to draw into it. When the Wimp or
a task draws in a window, the Wimp switches the VDU's output to the
backing's sprite with OS_SpriteOp 60. It then moves the graphics origin
so that screen coordinates land in the right place in the sprite. The
Font Manager, Draw, SpriteExtend, ColourTrans and the task's own
plotting all work as they did on the screen. While output is switched,
`ScreenStart` reports the sprite's pixels, as RISC OS has always done.

A few things are done to the pixels afterwards:

* **Holes.** Where a window is meant to show what is below it (the
  Pinboard's backdrop), the pixels are marked transparent, and the
  presenter shows layer 0 through them.
* **Opacity.** Some programs write their pixels with a top byte of
  zero, which in a backing means transparent. The ArtWorks renderer
  does this. After each rectangle a task redraws, the Wimp makes such
  pixels opaque, so the picture is solid.
* **Corners.** A theme with round corners has the corner pixels given
  the arc's coverage as alpha, so the window's edge is smooth over
  whatever is beneath it.

Backings are mapped into RISC OS's 4 GB arena only while they are
drawn into. The arena keeps a window of 128 MB for them at `&F0000000`,
holding the backings used most recently. When room is wanted the least
recently used goes out, and comes back, perhaps at a different address,
the next time anything draws in it. The presenter's own mappings are
never touched. Before this, every backing stayed mapped in a gigabyte
of its own. That gigabyte now belongs to dynamic areas.

---

## 4. Block v2

The Wimp and the presenter share a block of memory, block v2, after the
screen's pixels. It holds:

* a list of up to 128 surfaces, front to back: for each, its kind, its
  backing, where it is on the screen, which parts of it are visible,
  its corner radius, its shadow, and which rows of it changed;
* a list of the screen's damage, the rectangles that changed;
* a frame count, which is the block's lock;
* the presenter's acknowledgement of the last frame it took;
* the desktop's look (section 8).

The Wimp writes the block at its safe points: when it flushes its
redraws, at Wimp_Poll, inside an error box's loop, and at the VSync
while a single-tasking program runs. It makes the frame count odd while
it writes and even when it has done. The presenter reads the block and
takes it only if the count was even and did not change while it read.
It then writes the count into the acknowledgement. Until the
acknowledgement catches up, the Wimp adds new damage to the list
already there rather than starting a new one. So nothing is lost if the
presenter is a frame behind.

---

## 5. The presenter

The presenter is a Linux program, `rosgd-present`, started by `/init`
before RISC OS. It is the graphics card's only user. It opens the card
that has a monitor connected and sets the monitor's own mode. It then
draws with OpenGL ES through Mesa: on the NVIDIA T1000 with Mesa's
`nouveau` driver, and on Intel graphics with `iris`.

Each pass of its loop does this:

1. It reads block v2. If a new frame is there, it notes which surfaces
   changed and which rows of each.
2. It uploads only those rows of each changed backing to its texture.
   A window whose caret blinks costs one small strip, not the window.
3. It works out the part of the screen to draw again: this frame's
   damage, plus what changed since the buffer it is about to draw into
   was last used. The graphics driver says how old that buffer is.
4. Clipped to that, it draws the backdrop, then each surface from the
   back to the front: its shadow, a rounded box softened at the edges,
   then its picture. Then the Linux windows, then the pointer.
5. If the desktop has a look (section 8), the frame is drawn into a
   texture first and shown through a last pass in the colours asked
   for.
6. It hands the buffer to the display, and the monitor shows it at its
   next refresh.

When nothing has changed the pass draws nothing and shows nothing. An
idle desktop costs the graphics card very little. A window dragged
across the screen costs a few rectangles a frame. On the T1000 PC a
drag takes one to three per cent of the processor that runs RISC OS.

The presenter also sets the monitor's mode again when a DisplayPort
link is lost and found. It does this on Ctrl-Alt-F4, and two and a half
seconds after it starts, because a panel that is asleep at power-on
can come up with no picture.

---

## 6. From a task's drawing to the screen

This is the whole path for a task redrawing part of its window, for
example because its text changed.

1. The task calls Wimp_ForceRedraw, or the Wimp decides itself that
   part of the window must be drawn again. The Wimp notes the rectangle
   in the window's own list of invalid areas. Because the window has a
   backing, other windows are not involved.
2. At the task's next Wimp_Poll the Wimp returns a Redraw_Window_Request.
3. The task calls Wimp_RedrawWindow. The Wimp draws the window's
   furniture if it needs it, then switches the VDU's output to the
   backing and returns the first rectangle.
4. The task draws, as it always has, in screen coordinates. Its drawing
   lands in the backing.
5. The task calls Wimp_GetRectangle. The Wimp ends the drawing for that
   rectangle and makes any transparent pixels opaque. It records the
   rectangle as the backing's damage and returns the next.
6. When the task has drawn every rectangle, it polls again. At the
   flush, the Wimp writes block v2. It gives the window a new frame,
   with the damaged rows, and sets the corner alpha if the theme wants
   it.
7. The presenter takes the frame, uploads the damaged rows to the
   window's texture and draws the part of the screen they cover, with
   everything that overlaps it, in order.
8. The monitor shows it at its next refresh.

Moving a window involves no task at all. The Wimp changes the window's
position in block v2, and the presenter draws the backing in its new
place. The task is not asked to redraw, because its picture was never
lost.

Other kinds of surface take shorter paths.

* **A virtual display** (a task window's or GraphTask's) tells the Wimp
  when it changes. At its next safe point the Wimp plots the changed
  part into the window's backing at 32 bits a pixel, through a colour
  table if the display has fewer colours. Steps 6 to 8 follow.
* **A Linux window** never touches RISC OS's drawing. The compositor
  sends its buffer and the rectangles that changed straight to the
  presenter. The Wimp owns only the window's frame and its place in
  the stack.
* **The clock and the monitor** send nothing at all. The presenter draws
  them itself, at the rate each asked for, from a few bytes of data in
  the table.

---

## 7. A screen mode of a task's own

Under RISC OS a task never had a screen mode of its own. The desktop had
one, and every task shared it. That is still so for Wimp tasks: they
draw into backings at the desktop's size and in 16 million colours, and
ColourTrans finds their colours there.

A task can have a virtual display, which is a screen of its own in a
mode of its own. GraphTask and task windows use them. The task's whole
VDU state is its own: its mode, palette, cursors, text and graphics
windows, and two screen banks. The state is swapped in when the task
runs and out when the Wimp or background work runs. Its screen memory
is a dynamic area holding a sprite, at any depth from two colours to 16
million. The Wimp turns it into 32 bits a pixel when it plots it into
the window's backing. So a MODE 12 program in a GraphTask window reaches
the graphics card already in true colour.

---

## 8. Everything under the Wimp

On the composited desktop nothing reaches the screen except through a
surface. So we decided that BOX's desktop is a Wimp desktop and nothing
else: every program runs under the Wimp, in a window. This settled
several questions at once.

* **F12** opens a task window, as Shift-F12 does. The old command line
  at the bottom of the screen drew into layer 1, which is not shown.
  BASIC runs perfectly well in a task window.
* **The Task Manager's** "Exit desktop" is now "Restart desktop". There
  is nothing outside the desktop to exit to.
* **A program that is not a Wimp task**, started from the desktop (a
  BASIC program double-clicked in a Filer window, say), is given a
  virtual display of its own the first time it prints or changes mode.
  The Wimp shows the display in a window titled with the command. If the
  program printed first, the display is in the screen's own mode, so
  what the program read of the screen stays true. If it changed mode,
  the display is in the mode it asked for, and the desktop's mode is
  left alone. As on RISC OS, such a program has the machine to itself
  until it ends. Then the window's title asks for SPACE, and the window
  goes.
* **A BASIC text file** run from the desktop runs in a GraphTask window
  instead. That is a task window with a virtual display, so the desktop
  goes on while the program runs. The window can be moved and closed,
  and keys go to the program while the window has the caret. This suits
  the examples on the disc, which take time to calculate.
* **A desktop of fewer colours**, chosen in the Display Manager or by
  `*WimpMode`, is set at the size asked for in 16 million colours.
  Every task draws in true colour as before. The depth that was asked
  for goes to the presenter as the desktop's look, and its last pass
  shows the whole screen in those colours. Sixteen colours means the
  Wimp's sixteen. Two hundred and fifty-six means RISC OS's own palette,
  whose guns have two bits each and a tint shared by all three. Greys
  and 4096, 32K and 64K colours are made by cutting the guns' bits. The
  nearest colour is chosen, as ColourTrans would choose it, with no
  dither, so ArtWorks on a 256-colour desktop bands just as it always
  did. The Display Manager ticks the depth that was chosen.
* **Programs that write screen memory directly** are not supported. A
  program that changes mode first gets a virtual display, and its
  screen memory is then its own.

---

## 9. Vector icons

RISC OS icons are sprites, drawn for one resolution. Scaled to another
they go blocky or blurred. BOX can now use vector icons as well. These
are Haiku's HVIF format, a compact description of shapes, gradients and
outlines, which can be drawn at any size.

The VectorIcons module holds Haiku's own icon library, compiled for
BOX. `*IconSprites <file>` loads the sprite file as it always has, then
looks for a vector icon file of the same name with `V` added. Each
vector icon in it is drawn into a 32-bit sprite with alpha at the
desktop's resolution, and put in the Wimp's sprite area under its usual
name. Programs ask for icons by name as they always have and never know
the difference. At every mode change the icons are drawn again at the
new resolution.

There are three layers of vector icons:

* the Wimp's own,
* the icon set the theme names (its `[Theme] Vectors` key),
* the theme directory's own `SpritesV`.

A later layer replaces an earlier one name by name. Four sets are on
the disc, called BOX, Win2K, Pinstripe and Cube. Cube draws the BOX art
on dark plinths, in the manner of NeXT. The art comes from Haiku and
Fluent (MIT licence) and Tango (public domain). We rejected sets under
licences we could not ship.

One rule applies to every set. RISC OS's standard icons, such as Edit,
Paint, Draw, their files, the disc drives, `!Boot` and `!System`, look as
Acorn drew them. They are traced from RISC OS 5.30's 180 dpi originals
into vectors, without restyling, so they keep their shape at any size
and still look like RISC OS.

---

## 10. Themes, old and new

BOX's Wimp draws its windows in one of two ways, and a theme chooses
which.

**Sprite themes** are RISC OS's own. The window furniture is drawn from
tool sprites, as in every RISC OS since 3.5. RISC OS 5.30's themes load
as they are. Their `Sprites`, `Sprites22` and `Tools` files are used
unchanged, and where a set has alpha-masked icons (`SpritesA2`) those
are preferred. A sprite theme looks as it did on RISC OS 5.30, and its
icons are left alone: the Wimp's own vector icons stand aside under it.

**Painted themes** describe the furniture instead of supplying
pictures. A theme file gives fills (plain colours, gradients, stripes,
ridges), the shape of the title bar's buttons, the style of scroll bars
and frames, menu cells, dialogue buttons, option and radio icons,
writable fields, title fonts, and the corner radius and shadow. The Wimp
paints from the description at any resolution. Graphite, Paper, Nord,
HighContrast, Archimedes, Cube, Pinstripe and AcornFlat are on the disc.
`!Appearance`, in the ROM's Apps, chooses among them, and
`*WimpTheme` changes the theme while the desktop runs.

On the composited desktop both kinds are drawn into the window's
backing in the same way, and the presenter treats them alike. Round
corners and shadows come from the theme: the corners as alpha in the
backing, the shadows as the presenter's softened boxes. Neither is
painted into the pixels around the window, so a window can be dragged
over anything without leaving a mark.

---

## 11. What is left

The composited desktop is the default on the PC builds. These are the
known gaps:

* A program that is not a Wimp task still holds the whole desktop while
  it runs, as on RISC OS, unless it is a BASIC text file run in
  GraphTask.
* Programs that draw only by writing screen memory, without a mode
  change, show nothing.
* The window's chrome and its work area share one backing. Separating
  them would let a window scroll by moving its picture on the graphics
  card, with no redraw at all. That is the next step for performance.
* The Mac and QEMU builds still composite the old way, on one screen.
