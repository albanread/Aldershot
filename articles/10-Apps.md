# Apps for BOX

*18 October 2026 (revised 1 November 2026)*

**Note: this is not RISC OS.** BOX is a RISC OS window manager and application
environment running on a Linux kernel. It is not RISC OS, it is not a port
of RISC OS, and it does not claim to be either. It aims to be reasonably
compatible: it runs translated applications, and some emulated ones.

## Introduction

RISC OS has good applications. Some of them were created twenty or thirty
years ago, and a few are still maintained. What they cannot do is open all
of the files we need today: a `.docx` from a colleague, an SVG from a
drawing tool, a PDF from a bank, an `.xlsx` from an accountant.

Writing a word processor is hard. Writing one that reads Word's files
correctly is far harder, and it is not the interesting part. The file
formats are enormous, they are not really documented by their
specifications, and getting them right is years of unglamorous work that
has already been done, several times over, in the open.

So BOX's own applications are split in two. The part that understands the
document is Rust code, large and complicated, most of it written in the
open by other people, of which we keep our own copy. The part the person
uses is a RISC OS application, small and plain and written in Mojo, which
behaves as a RISC OS application should.

When this article was first written there were six applications. There
are now seventeen, and the code under them has been reorganised. This
revision describes them as they are on 1 November 2026.

This article covers:

- what the applications are;
- how an application is put together, and the line down the middle of it;
- why the user interface is written in Mojo;
- why the document code is Rust, and the three layers it is in;
- how the two halves talk, and how the picture reaches the screen;
- the shell that makes them all siblings;
- where they stand.

All of them are work in progress.

---

## 1 The applications

Sixteen do work, and the seventeenth is the container the others are made
from. They are in Apps on the Host disc.

**Documents.**

| Application | What it is | Opens | Saves and exports |
| --- | --- | --- | --- |
| **!Scribe** | a word processor | Word documents (`.docx`, and the older `.doc`) | `.docx`; PDF, Rich Text, OpenDocument, plain text, HTML |
| **!Ledger** | a spreadsheet | Excel workbooks (`.xlsx`), CSV | `.xlsx`; CSV, HTML |
| **!Folio** | a PDF reader and editor | PDF; a picture or text file becomes a new PDF | PDF; its text, a page as a sprite |
| **!Present** | a presentation program, which shows its slides on the whole screen | PowerPoint (`.pptx`) | `.pptx`; PDF, a slide as PNG or JPEG |
| **!Publish** | page layout: pages, frames, stories that run from frame to frame | its own layouts, IDML; pictures and PDF pages placed | its own; PDF, IDML, EPUB, a page as PNG |
| **!Draft** | Markdown with diagrams, drawn as a page | Markdown | Markdown; Draw file, web page, sprite |

**Pictures.**

| Application | What it is | Opens | Saves and exports |
| --- | --- | --- | --- |
| **!Trace** | a drawing program | SVG, PostScript and EPS, DXF, Illustrator; Draw and ArtWorks files dropped on it | SVG; Draw, ArtWorks, PDF, EPS, DXF, PNG, JPEG |
| **!Palette** | a photo editor | PNG, JPEG, TIFF, GIF, BMP; sprites dropped on it | PNG; sprite, JPEG, TIFF, GIF, BMP, WebP, Photoshop |
| **!Canvas** | a painting program, started from nothing, with brushes that behave like paint | PNG dropped on it | PNG |
| **!FloorPlan** | plans of houses and small offices: walls, doors, windows, rooms, furniture | its own plans | its own; Draw file, PDF, DXF, SVG, PNG |

**Sound, programs and work.**

| Application | What it is | Opens | Saves and exports |
| --- | --- | --- | --- |
| **!Studio** | a multitrack studio: clips on tracks along a timeline | WAV, AIFF and FLAC dropped on it | its own sessions; the mix as WAV, AIFF or FLAC |
| **!Code** | the programmer's editor, with language servers, Build and Run, and a debugger | Mojo, BASIC, C and other text, Tcl, Lua, Python, Modula-2, BCPL, Scheme, Forth, Smalltalk; a directory as a project | the file |
| **!Workflows** | Tcl workflows: the steps a document goes through, drawn | its own workflows | its own; web page, text |

**Games.**

| Application | What it is |
| --- | --- |
| **!Micropolis** | the city simulation, from the 2008 GPL release of the original game's source |
| **!Tower** | a tower-building game |
| **!Antiquity** | a 3D strategy game, still in development |

**The container.**

| Application | What it is |
| --- | --- |
| **!Shell** | the container the others are, with an engine that does nothing |

They are not ports. Apart from the simulations inside the games, nothing
here was written for RISC OS before and carried across. They are new
applications, which is why they can read these formats at all.

---

## 2 The shape of an application

Each one is two programs.

```
  +---------------------------------------------------------------+
  |  !Scribe                      a Wimp task, written in Mojo    |
  |                                                               |
  |  the window, the title bar, the scroll bars, the toolbar,     |
  |  the menus, the tool palette, the rulers in millimetres,      |
  |  the save box, the icon bar icon, interactive help            |
  +---------------------------------------------------------------+
                |                                   ^
                |  "key 393 2"                      |  "damage 0 0 794 1123"
                |  "press 210 480 0"                |  "title Letter"
                |  "save /host/Letter/docx"         |  "modified 1"
                v                                   |
  +---------------------------------------------------------------+
  |  ROSApps                      a module in the ROM, in C       |
  |                                                               |
  |  starts the engine, holds the socket, holds the page buffer,  |
  |  binds the buffer to the window, turns damage into repaints   |
  +---------------------------------------------------------------+
                |                                   ^
                |  a Unix socket                    |  a shared page buffer
                v                                   |
  +---------------------------------------------------------------+
  |  Engine                       a 64-bit Linux program, in Rust |
  |                                                               |
  |  the document itself: reading the file, laying out the text,  |
  |  the undo history, and drawing the page into the buffer       |
  +---------------------------------------------------------------+
```

The line down the middle is the important part. **Everything above it is
RISC OS and knows nothing about document formats. Everything below it
understands documents and knows nothing about RISC OS.** Neither half
can confuse the other, because the only things that cross are short lines
of text and a buffer of pixels.

The engine is a file in the application directory, one for each processor
(`Engine-aarch64` and `Engine-x86_64`). It is kept in the application's
hidden `.engine` directory, which the Filer does not show, so that it
cannot be deleted by accident. `!Code` keeps its language servers there
too.

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
all. For the kind of code a Wimp task is (short-lived strings, a few
lists, a lot of calls into the operating system) that is the right
trade: the safety is useful, and the fight is not.

**It is not unfriendly.** This matters more than it sounds. A RISC OS
application's front end is exactly the sort of code somebody should be
able to read, copy and change for an application of their own. A language
that takes a fortnight to become productive in is a poor choice for that,
however good it is.

The applications are compiled by the box's own `*Mojo` the first time
each is run, by its `!Run` file, so the source in the application
directory is the program.

The BOX side of this is the `riscos` package, and `desktop.mojo` inside
it, which gives a Wimp task its application object, its windows, its
menus and its redraw. It is described in
*The Mojo riscos Library*; *Mojo on BOX* describes the language itself.

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

Rust is the right language for this for the reason it is the wrong one
for the front end: this is code that parses untrusted files from the
outside world, where a mistake is a security hole, and where the
compiler's strictness is worth the trouble it causes.

The engines are also 64-bit code. That is not a problem here, because an
engine is a separate Linux program and not part of RISC OS at all. It
never sees a RISC OS address. This is the same arrangement the browser
uses, and it is why a 32-bit operating system can have a 64-bit document
engine behind it.

### The three layers

An engine is built in three layers.

**The libraries** are large Rust libraries that already understand these
formats: the ArtCraft projects (WordCraft, VectorCraft, PhotoCraft,
gridcraft, PDFCraft, DeckCraft, DesignCraft, CADCraft and SoundCraft),
written in the open under permissive licences. We keep our own copy of
them, taken at the revisions the engines were built from, so that a build
does not depend on anybody else's repository and the libraries are ours to
change. From each we use the document model, the renderer and the file
formats. Their user interfaces are not used.

**The content crates** are ours. Each holds one kind of document as a
RISC OS user has it, whatever application shows it: how it is opened (a
file known by its RISC OS type), made, saved, exported and drawn for the
printer. Where a library does the work, the crate is our interface to it.
There are nine:

| Crate | The document |
| --- | --- |
| `text` | WordCraft's documents |
| `sheet` | gridcraft's workbooks, their number formats and the text of their cells |
| `pdf` | PDFCraft's documents, through its automation layer |
| `slides` | DeckCraft's presentations |
| `layout` | DesignCraft's layouts and their exports |
| `plan` | plans: walls, rooms, furniture, and their drawings and files |
| `sound` | an arrangement, its sounds, its mix and its picture |
| `picture` | sprite files of every kind read, and written at 32 bits or 256 colours; any picture file read |
| `drawing` | Draw files written and read, ArtWorks files read, RISC OS outline fonts |

**The engine** is the application's: its commands, its menus, tools and
panes, its view, and the line protocol. What every engine shares (the
socket, the shared picture and the handover of its damage, the frame it
draws into, RISC OS Latin-1 and the escapes text crosses the socket in, a
file's name as HostFS gives it) is one more crate, `engine`.

| Application | Content crate | Library |
| --- | --- | --- |
| !Scribe | `text` | WordCraft |
| !Ledger | `sheet` | gridcraft |
| !Folio | `pdf` | PDFCraft |
| !Present | `slides` | DeckCraft |
| !Publish | `layout` | DesignCraft |
| !Draft and !Workflows | | selkie, for Markdown and Mermaid diagrams |
| !Trace | `drawing` | VectorCraft |
| !Palette | `picture` | PhotoCraft |
| !Canvas | | PhotoCraft's brush engine |
| !FloorPlan | `plan` | CADCraft's lower crates |
| !Studio | `sound` | SoundCraft |
| !Code | `picture` | none: the editor, its highlighting and its use of language servers are ours |
| !Micropolis | | the Micropolis simulation, in C++ |
| !Tower | | tower-together's simulation, in Rust |

!Trace, !Palette and !Canvas open the world's formats through their
library directly; their content crates are RISC OS's own formats, Draw and
ArtWorks files and sprites.

All of it is one cargo workspace with one lock file, so every engine is
built from the same version of every crate the engines share. That needed
the libraries to agree, and on PDF they did not: there were three copies
of the same PDF crates, each patched its own way. Now there is one.

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
ordinary RISC OS sprite area. The engine draws the document into it. The
module maps the same memory into the arena, so the Wimp can see it as a
sprite, and binds it to the application's window with
`Wimp_Extend &5200`.

From that moment the window is an ordinary backed window whose work area
the Wimp paints from that sprite. The application does not draw the
document at all. When a `damage` line arrives, the module turns it into
`Wimp_Extend &5202`, which paints that rectangle into the window's
backing at once. Only the changed rows move.

The engine is told `shown` when the Wimp has painted, and may not write
into the buffer again until then. That one rule is what stops the two
halves drawing over each other.

### A picture of the whole, or of the view

The first applications drew the whole document into the sprite, at the
current scale, and let the Wimp scroll it. That does not work for a
spreadsheet of a hundred thousand rows, a PDF of three hundred pages or a
timeline of an hour, so most applications now draw only what is in view.
The sprite is the size of the window's visible area and is pinned to it,
so scrolling does not move it. The window's extent still stands for the
whole document, so the scroll bars mean what they always mean. When the
window is scrolled or resized the shell tells the engine where the view
is (`view SX SY`), and the engine moves the pixels it has and draws only
what is new. !Ledger, !Folio, !Publish, !Studio and !Code work this way.

### The games

The games do not send pixels at all where they need not. !Micropolis
gives the presenter its map of tiles and the pictures of the tiles, and
the graphics card draws the city; !Tower gives it the tower; !Antiquity
gives it a 3D scene. The presenter is the program that composites the
desktop: Metal draws it on a Mac, and GLES on a PC. Where there is no
presenter that can draw a game, its engine draws the picture into the
sprite, as the other applications do, and says so.

[The Compositing Wimp](8-Compositing.md) describes what happens to the window after that.

---

## 6 The shell

All seventeen applications are the same program.

`rosapp.mojo` is the shell. It provides:

- the document window and its toolbar, and the rulers in millimetres;
- the tool palette and the inspector, which sit outside the window and
  tuck in over the page when the window is pushed against the edge of the
  screen, and the properties pane on the right;
- the shared menu entries, the icon bar menu and its Info box, Choices
  and interactive help;
- more than one document at a time, each in its own window with its own
  engine, and the Documents entry on the icon bar menu that lists them
  (!Code has one window with a tab for each file instead);
- loading a file that is double-clicked or dropped on the icon, and
  placing one dropped on a window;
- the save box, and Export, which saves in another format;
- the RISC OS clipboard;
- the question and choice boxes an engine asks for, such as a spelling
  check's;
- printing, which the engine draws page by page for the printer driver.

An application is that file with a configuration (its name, its engine,
its tools, its menus, the file types it opens and exports) and whatever
little is genuinely its own.

This is deliberate. Applications that are siblings should *look* like
siblings, and the reliable way to achieve that is for them to be the same
code rather than for seventeen applications to follow the same Style Guide
carefully. A fix to the save box fixes it in all of them. !Shell is that
container with an engine that does nothing, which is both a test and a
starting point for the next one.

---

## 7 Where they stand

Every one of these applications is work in progress. Each opens its
documents, edits them and saves them. The document applications have each
been tried in the Apple silicon box and in the Intel box, opened as a
person opens it, with an edit that changes its page, and a change to their
engines is checked by a harness that drives it through every tool, every item of its menus,
saving, each export and opening again, and compares what it says and
draws with what it did before.

That is not the same as being finished. There are things still to do in
each of them, some of them large. Two affect all of them:

- **Printing** is written, but it can stop the desktop while a document
  prints, and is being reworked to print in the background.
- **Saving** works only on HostFS, for now.

---

## 8 What this is for

The point is not that BOX needs a word processor; Ovation Pro and
PipeDream both run, and are better at what they do. The point is that a
RISC OS desktop should be able to open the files the rest of the world
sends, in applications that feel like RISC OS applications and not like
something ported from elsewhere with its own idea of how a menu works.

The split is what makes that affordable. The hard, large, unglamorous
half is library code written in the open by people who care about those
formats. The half that decides what the thing feels like to use is small,
readable, and ours.
