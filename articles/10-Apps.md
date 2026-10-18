# Apps for BOX

*18 October 2026*

**Note — this is not RISC OS.** BOX is a RISC OS window manager and application
environment running on a Linux kernel. It is not RISC OS, it is not a port
of RISC OS, and it does not claim to be either. It aims to be reasonably
compatible: it runs translated applications, and some emulated ones.

## Introduction

RISC OS has good applications. Some of them were created twenty or thirty
years ago, and a few are still maintained. What they cannot do is open all
of the files I need today: a `.docx` from a colleague, an SVG from a
drawing tool, a PDF from a bank, an `.xlsx` from an accountant.

Writing a word processor is hard. Writing one that reads Word's files
correctly is far harder, and it is not the interesting part. The file
formats are enormous, they are not really documented by their
specifications, and getting them right is years of unglamorous work that
has already been done, several times over, in the open.

So BOX's own applications are split in two. The part that understands the
document is existing Rust code, large and complicated and not ours. The
part the person uses is a RISC OS application, small and plain and written
in Mojo, which behaves exactly as a RISC OS application should.

This article covers:

- what the applications are;
- how an application is put together, and the line down the middle of it;
- why the user interface is written in Mojo;
- why the document code is Rust;
- how the two halves talk, and how the picture reaches the screen;
- the shell that makes them all siblings.

---

## 1 The applications

Six, as of this writing. Five do work, and the sixth is the container the
others are made from.

| Application | What it is | Opens and saves |
| --- | --- | --- |
| **!Scribe** | a word processor | `.docx` |
| **!Trace** | a drawing program | SVG, PostScript and EPS, Draw files |
| **!Palette** | a paint program | PNG, JPEG |
| **!Ledger** | a spreadsheet | `.xlsx`, CSV |
| **!Folio** | a PDF reader and editor | PDF |
| **!Shell** | the container the others are, with nothing in it | — |

They are not ports. Nothing here was written for RISC OS before and
carried across. They are new applications, which is why they can read
these formats at all.

---

## 2 The shape of an application

Each one is two programs.

```
  +---------------------------------------------------------------+
  |  !Scribe                      a Wimp task, written in Mojo     |
  |                                                                |
  |  the window, the title bar, the scroll bars, the toolbar,      |
  |  the menus, the tool palette, the rulers in millimetres,       |
  |  the save box, the icon bar icon, interactive help             |
  +---------------------------------------------------------------+
                |                                   ^
                |  "key 393 2"                      |  "damage 0 0 794 1123"
                |  "click 210 480 0"                |  "title Letter"
                |  "save /host/Letter/docx"         |  "modified 1"
                v                                   |
  +---------------------------------------------------------------+
  |  ROSApps                      a module in the ROM, in C        |
  |                                                                |
  |  starts the engine, holds the socket, holds the page buffer,   |
  |  binds the buffer to the window, turns damage into repaints    |
  +---------------------------------------------------------------+
                |                                   ^
                |  a Unix socket                    |  a shared page buffer
                v                                   |
  +---------------------------------------------------------------+
  |  Engine                       a 64-bit Linux program, in Rust  |
  |                                                                |
  |  the document itself: reading the file, laying out the text,   |
  |  the undo history, and drawing the page into the buffer        |
  +---------------------------------------------------------------+
```

The line down the middle is the important part. **Everything above it is
RISC OS and knows nothing about document formats. Everything below it
understands documents and knows nothing about RISC OS.** Neither half
can confuse the other, because the only things that cross are short lines
of text and a buffer of pixels.

---

## 3 Why Mojo for the part the person sees

The application half is a Wimp task like any other. It calls
`Wimp_Initialise` and `Wimp_Poll`, it has a window definition and a menu
block, and it tries to follow the Style Guide. It is also easy to change,
and to add new features to a little at a time. There is nothing exotic
about it. The question is only what language to write it in, and we chose
Mojo.

The reasons are plain ones.

**It reads like what it does.** The Wimp's poll loop, its windows and its
menus are a lot of small, fiddly, concrete steps. Code for them should be
readable by somebody who did not write it, a year later. Mojo is
Python-shaped: a person who has written a BASIC program or a Python script
can read `rosapp.mojo` and follow what happens when a menu item is chosen.

**It is fast.** It compiles to native code, so a Wimp task written in it
is not slower than one written in C. There is no interpreter and no
garbage collector pausing in the middle of a redraw.

**Its memory management is Rust's good idea without Rust's hard edges.**
Mojo has ownership and borrowing, so a program that compiles is not
leaking memory or using it after it is freed. But it does not demand that
every program be arranged to please the compiler before it will build at
all. For the kind of code a Wimp task is — short-lived strings, a few
lists, a lot of calls into the operating system — that is the right
trade: the safety is useful, and the fight is not.

**It is not unfriendly.** This matters more than it sounds. A RISC OS
application's front end is exactly the sort of code somebody should be
able to read, copy and change for an application of their own. A language
that takes a fortnight to become productive in is a poor choice for that,
however good it is.

The BOX side of this is a `riscos` package for Mojo, and `desktop.mojo`
inside it, which gives a Wimp task its application object, its windows,
its menus and its redraw. A Mojo program on BOX reaches the operating
system through it, one SWI at a time, as a C program would.

---

## 4 Why Rust for the part that reads the file

The opposite argument applies underneath.

Reading `.docx` properly means a ZIP reader, several XML schemas, a style
and numbering model, a layout engine that breaks lines and pages the way
Word does, and a font stack. PDF means an interpreter for a page
description language, several compression schemes, colour spaces and
encryption. These are not weekend projects, and a half-correct
implementation is worse than none: it opens the file and shows the wrong
thing.

That work exists, in the open, under permissive licences. The engines are
built on it:

| Engine | Built on |
| --- | --- |
| !Scribe | WordCraft |
| !Trace | VectorCraft |
| !Palette | PhotoCraft |
| !Ledger | gridcraft |
| !Folio | PDFCraft |

They are large Rust libraries that already understand these formats. Rust
is the right language for them for the reason it is the wrong one for the
front end: this is code that parses untrusted files from the outside
world, where a mistake is a security hole, and where the compiler's
strictness is worth the trouble it causes.

They are also 64-bit code. That is not a problem here, because the engine
is a separate Linux program and not part of RISC OS at all. It never sees
a RISC OS address. This is the same arrangement the browser uses, and it
is why a 32-bit operating system can have a 64-bit document engine behind
it.

---

## 5 How the halves talk

The `ROSApps` module in the ROM is the only code that touches both sides.
It is about as small as it sounds.

When an application starts, it calls `ROSApps_Open` with the name of its
engine. The module starts the engine as a Linux process with two open
descriptors: a Unix socket, and a buffer for the page.

**The socket carries lines of text.** The application sends things like
`key 393 2`, `press 210 480 0`, `tool pen`, `open /host/Letter/docx`,
`save /host/Letter/docx`. The engine sends back `title Letter`,
`modified 1`, `doc 794 1123`, `error ...`, and `damage` for each
rectangle of the page it has redrawn. That is the whole protocol: plain
text, one line at a time, easy to read in a log and easy to test from a
script.

**The buffer carries the picture.** It is a `memfd` laid out as an
ordinary RISC OS sprite area. The engine draws the document's pages into
it. The module maps the same memory into the arena, so the Wimp can see
it as a sprite, and binds it to the application's window with
`Wimp_Extend &5200`.

From that moment the window is an ordinary backed window whose work area
the Wimp paints from that sprite. The application does not draw the
document at all — there is nothing in it to draw but the rulers. When a
`damage` line arrives, the module turns it into `Wimp_Extend &5202`, which
paints that rectangle into the window's backing at once. Only the changed
rows move.

The engine is told `shown` when the Wimp has painted, and may not write
into the buffer again until then. That one rule is what stops the two
halves drawing over each other.

[The Compositing Wimp](8-Compositing.md) describes what happens to the
window after that.

---

## 6 The shell

All six applications are the same program.

`rosapp.mojo` is the shell: the document window and its toolbar, the
rulers in millimetres, the tool palette and the inspector that tuck in
over the page when the window is pushed against the edge of the screen,
the shared menu entries, the icon bar menu and its Info box, Choices,
interactive help, and loading a file dropped on the icon. An application
is that file with a configuration — its name, its engine, its tools, its
menus and the file types it opens — and whatever little is genuinely its
own.

This is deliberate. Applications that are siblings should *look* like
siblings, and the reliable way to achieve that is for them to be the same
code rather than for three people to follow the same Style Guide
carefully. A fix to the save box fixes it in all six. !Shell is that
container with an engine that does nothing, which is both a test and a
starting point for the next one.

---

## 7 What this is for

The point is not that BOX needs a word processor; Ovation Pro and
PipeDream both run, and are better at what they do. The point is that a
RISC OS desktop should be able to open the files the rest of the world
sends, in applications that feel like RISC OS applications and not like
something ported from elsewhere with its own idea of how a menu works.

The split is what makes that affordable. The hard, large, unglamorous
half is somebody else's code, maintained in the open by people who care
about those formats. The half that decides what the thing feels like to
use is small, readable, and ours.
