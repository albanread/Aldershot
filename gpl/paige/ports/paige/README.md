# Paige: the rich-text engine for the box's word processor

Paige is the classic rich-text engine -- styled text, paragraphs, tables,
embedded objects, text flowing through container shapes, undo, its own
document format with RTF/HTML import-export -- written in portable C by
DataPak Software (1993-2005), revived and open-sourced by Team HERMES as
[HERMES-Paige] under LGPL-2.1.  The pristine clone, with our port work
committed on top, is `third_party/Paige` (see its CATALOGUE entry); this
directory is the port: the platform header, the engine patch, the build.

## The state of the port (5 October 2026)

The engine and its RISC OS machine layer build for both boxes, as one
archive a C application links against the box's C library:

    sh ports/paige/build.sh                  # from rosgd/: x32
    ARCH=aarch64 sh ports/paige/build.sh     # A64X32

gives `build/ports/paige/libpaige.a` (or `build/aarch64/...`): PGSOURCE,
PGHLEVEL and PGDEBUG (about 56,000 lines), PGPLATFO/PGMEMMGR.C, and the
machine layer `rosc/`, all compiled with capps.mk's flags against the
box's C headers.  `PGPLATFO/PGROSC.H` is force-included: errno names and
the Mac fixed-point trio in plain C.  `!Write` (ports/write) is the
application built on it.

The port's changes to the engine are committed in `third_party/Paige`
(a clone of HERMES-Paige), all under `ROSC_COMPILE`/`ROSC_PLATFORM`, and
carried here as one diff, `patches/engine-32bit-and-platform.patch`.  The
build compiles `.cache/paige`, HERMES-Paige at `cddd954` with that patch,
made again whenever the patch changes; `PAIGE=dir` compiles another tree
as it stands.  The changes:

- `PGHEADER/PGMTRAPS.H`: memory.  A memory_ref is a pointer to a cell
  holding its block's address, as a Mac Handle is, so a block may move
  when it grows and the ref stays good (`rosc/roscmem.c`, over malloc).
- `PGPLATFO/PGMEMMGR.C`: the master list's casts for 32 bits.  Its
  `next_master` is size_t, so the test for a full list (`< 0`) was never
  true: past 256 refs each new one was stored at index -1 and on into
  the zero words after the list, over the C library's heap (in !Write,
  SQLite's connection, which then called a null trace hook).  It is
  tested against `(size_t)-1` now, and DisposeMemory and DetachMemory
  give the slot back under ROSC_PLATFORM as on the Mac.  The other
  `size_t < 0` tests (pgFixOffset, pgWalkStyle, pgEmbed) are cast to
  long; clang's -Wtautological-unsigned-zero-compare finds no more.
- `PGHEADER/PAIGE.H`: text offsets signed again.  HERMES's 64-bit pass
  made `style_run.offset`, `select_pair` and `t_select.offset` size_t;
  the engine's arithmetic on them (an offset less one, compared) went
  unsigned with it, and the first insertion into a new document walked
  off the end of its style runs.  `pg_text_offset` is `long` under
  ROSC_PLATFORM; on an ILP32 target the layout does not change.
- `PGSOURCE/PGCLIPBD.C`: `apply_text_styles` sets `start_target` again.
  The same pass rewrote `target_run = start_target = InsertMemory(...)`
  with a cast and lost the second assignment; undoing typing or a paste
  read through a null pointer.

`patches/engine-32bit-and-platform.patch` is the whole as a diff against
upstream, for reference.

### The machine layer (rosc/)

`rosc/pgrosc.c` is Paige's machinespecific file: the procs the engine
calls through its globals and style records.

- Fonts: the Font Manager.  A style's handle is kept in its machine_var.
  Characters are measured once a handle, Font_StringWidth of each
  Latin-1 character, and a run's positions are the running sum in
  millipoints, each rounded once, so rounding does not accumulate.
- Measuring: Paige's measure procs fill length + 1 positions, [0] the
  run's start as the caller set it and [i + 1] the right edge of
  character i.  (HERMES's portable layer wrote [i]; every line's layout
  was one character out.)
- Drawing: Font_Paint at OS-unit coordinates, ColourTrans for the
  style's colour, underline and strike-through as lines; the pen's end
  goes in the draw position's `to`, as the Windows layer has it.  A draw
  happens only when the application has set a target
  (`rosc_set_target`), so the engine lays out and measures without one.
- One Paige unit is one point (device resolution 72); the screen is 5/2
  OS units the point.  The device's scale is 0, no scaling (a scale is a
  numerator and a denominator in halves of a word: 1/0 divides by zero).
- The caret and the selection are the application's: the cursor and
  hilite procs keep Paige's flags only.
- Files: a pg_file_unit is a RISC OS file handle.  The pgOS read and
  write procs and the file primitives (position, extent, read, write)
  are OS_Args and OS_GBPB.
- Regions are rectangle lists; the Unicode conversions are PGOSUTL.C's.

### The RTF codecs (PGTXR)

HERMES made PGTXR C++, of the plainest kind: classes and virtual
functions, new and delete, no library.  It is compiled as C++ (gnu++14,
no exceptions, no RTTI, `-fno-wchar` so wchar_t is the C headers' type as
in the C files), and `rosc/cxxrt.c` is all the runtime it needs: new and
delete over malloc, the pure-virtual trap.  Under ROSC_PLATFORM PG_TRY is
setjmp in C++ as in C (PGEXCEPS.H) -- the engine fails by longjmp, and
pg_fail_info is now the same in both.  Faults found in it, fixed in
`third_party/Paige`:

- the UNICODE build's RTF keyword table for destinations was an old
  list, three words short, so every one after them was written as
  another (`\stylesheet1` for `\rtf1`); it is the 8-bit table's now;
- the exporter copied each run's text with a count of characters as a
  count of bytes: half of every run was lost;
- `\ul0` turned italic off, not underline; `\ulnone` (RTF 1.5 on) was
  not known -- the keyword tables are alphabetical, and the matcher
  stops at the first word past the one it is looking for;
- a backslash before a line's end (`\par`, as macOS writes it) was
  dropped, and a line's end inside the font table went into the next
  font's name.

The machine layer's `pgCStrLength` counts pg_chars (the engine's C
strings are pg_char wide: font names, URLs), and `pgFixFontName` is
PGOSUTL.C's: a font chosen from the Font menu had been stored as "H".
Other systems' families are mapped to the Font Manager's when a font is
found: serifs to Trinity, sans serifs to Homerton, fixed pitches to
Corpus.

The HTML codecs had more of the same, all fixed there too: four of
PGHTMDEF.C's keyword tables were older than PGHTMDEF.H's enums (div,
style, class, embed, object, param, id and more had been numbered but
never listed, so `<li>` came out for `<html>`); the exporter wrote
characters with pgWriteByte, which in a UNICODE build is half of one
(every other character lost); characters were copied as bytes in eight
places (the title, attribute values -- `#c00000` read as `#c00` --
font names, alt text, file names); and the default fonts were C
literals cast to pg_char strings ("TmsNwRmn").  Under ROSC_PLATFORM the
defaults are Trinity and Corpus, and the body's size is HTML's own
default, 3 (12 point), not Paige's 10 point.

## What is not done

Printing (the print device procs are the screen's).  Pictures and
embedded objects.  Hyperlinks import as text.

## Licence

Paige is LGPL-2.1.  The obligation is the library's own code: the port
changes in `third_party/Paige` stay LGPL and publish with whatever links
them; callers across a SWI or link boundary are unaffected.

[HERMES-Paige]: https://github.com/nmatavka/HERMES-Paige
