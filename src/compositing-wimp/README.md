# The Compositing Wimp

This is the Window Manager as the PC builds of BOX run it, where every
window has a picture of its own and a separate program composites the
desktop on the graphics card. It is published whole, with the presenter
that draws it and the block format the two share.

It does **not** replace `src/box/modules/wimp`, which is the Window Manager
as the other builds run it, drawing into one screen in memory. The two are
the same Wimp with different output: the files here are that Wimp with the
surface, backing and presenter work added.

*The Compositing Wimp* in [articles](../../articles/8-Compositing.md)
describes how it works, and is the place to start.

## Licence

**Apache License 2.0** — see [LICENSE](LICENSE).

The Window Manager is RISC OS Open Ltd's. These files implement it for
BOX: written in C against RISC OS 5.30's behaviour and, where the exact
behaviour mattered, against RISC OS Open's own sources — `s/Wimp10` and
`Wimp02` are cited in the code where that was done. They therefore keep the
licence of the work they derive from, the Apache License 2.0, as the rest
of BOX's RISC OS code does.

The compositing work — a window's backing, the surfaces list, the damage,
the presenter, the shadows and the rounded corners — is the BOX project's
extension, published under the same licence so the whole is one thing.

One file, `wimp/backing.c`, carries an MIT identifier from when it was
written. It is published here under the Apache License 2.0 with the rest;
the MIT grant it carries is not withdrawn by that.

## What is here

| | |
| --- | --- |
| `wimp/` | the Window Manager: 29 files |
| `present/` | the presenter, the program that composites the desktop |
| `include/screen.h` | block v2, the format the two share |

### The Wimp

The files that make this the *compositing* Wimp:

| File | What it holds |
| --- | --- |
| `surface.c` | the surfaces list, the pages, the drag overlay, corner alpha, and which windows become surfaces |
| `backing.c` | making, freeing and switching output into a window's backing |
| `redraw.c` | the redraw scan and each window's own pending list |
| `input.c` | drags and drag boxes |
| `errorbox.c` | the error box as a surface |
| `shadow.c`, `shape.c` | shadow rectangles and round-corner geometry |

The rest are the Window Manager itself: windows, icons, menus, the caret,
messages, filters, templates, the icon bar, the command window, the
clipboard, fonts, sprites and the task list.

### The presenter

`present/rosgd-present.c` is the program that reads the surfaces list and
each window's backing and draws the desktop on the graphics card, with the
backdrop beneath and the pointer above. `copyto.c`, `build-in-vm.sh` and
`mktestroot.sh` are what build and install it.

### The shared format

`include/screen.h` is block v2: the surfaces, their visible rectangles,
their damage, and the seqlock that lets the presenter read them while the
Wimp writes them. Both sides include this one file, which is why it is
here rather than in either directory.

## How a window reaches the screen

1. A task draws as it always has — `Wimp_RedrawWindow`, `OS_Plot`,
   `Font_Paint`, `OS_SpriteOp`.
2. The Wimp switches VDU output into that window's backing, rectangle by
   rectangle, and records what changed.
3. At the next `Wimp_Poll` the Wimp publishes the surfaces list and the
   damage into block v2.
4. The presenter uploads the changed rows of each backing into a texture
   and draws the desktop.

A task is not changed by any of this and cannot tell the difference.

## Building it

These files are not a standalone library. They are part of `/init`, built
with the rest of BOX; `src/box` holds the rest of that source. The
presenter is a separate program, built in the box's Linux root with the
scripts in `present/`.
