# PipeDream 4.63 for the ROSGD box (x32) -- port notes

PipeDream 4.63 (tag `pipedream__2025_10_29__4_63_00`) built with the box's
clang -> roscc x32 pipeline, linked against the box's rlib.a and ROM C
library.  Status: **working** -- text view opens, the columns grid renders,
typing works (user-verified 29 September 2026); tested in full and
cross-checked against ARM PipeDream 4.62 on 5 October 2026 (below).

    sh tools/build.sh           # compile (148 units) and link
    build/!RunImage             # the x32 image

The app on the share: `rosgd/build/share/Apps/!PipeDream` (the real A72
install's !Boot, !Run adapted, Resources).  Run windowed:

    sh run/run-x86_64.sh        # desktop, PipeDream via ssh or double-click

## THE LESSON: be minimal when porting C apps

The first attempt rewrote the header layer -- a single-lineage wimp.h,
custom event structs, reinterpreted tboxlibs types, R0 set by hand in the
Wimp wrappers.  It compiled (148 units, zero errors) and was completely
wrong: no text displayed, typing crashed with `&288 Illegal window
handle`, and every bug chase through the SWI ring led back to struct
layouts that didn't match what the code expected.

The working build changed exactly three things from pristine:

1. **`COMPILING_WIMPLIB_EXTENSIONS`** for the `WimpLib/cs-*.c` files
   (the original Makefile's `CCflags_CSX` -- without it the cs-* files
   compile through the wrong cs-wimp.h branch and miss the tboxlibs types)

2. **`menuwarn`** not `menuwarning` in the ACW-patched wimp.h (the ACW
   patch names it `menuwarn`; 6.23 stock renamed it)

3. **`submenu` as a plain `wimp_menuptr`** in the ACW-patched wimp.h
   (event.c reads `m->data.menuwarn.submenu` directly; the 5_82-era
   union wrapping breaks that)

Everything else is the original source, unmodified, but for one fault
fixed in `pdriver.c`: the first printer driver load compared its name
with `current_driver_name` while that was still NULL.  On RISC OS 5.30
the read through NULL is harmless; on the box it aborts (issue #171).  The two header
lineages the original code expects were reconstructed from real files,
not reinvented:

- `WimpLib/wimp.h` = stock RISC_OSLib **5_82** (the tag the patches
  were made against) + ACW's `rlib_patch/wimp.h` applied clean
- `pd/src/tboxlibs/wimp.h` = the **real ToolboxLib header** from the
  ROOL drop at `Sources/Toolbox/ToolboxLib/toolboxlib/h/wimp` (1100
  lines; has every Wimp* type, Wimp_M* macro, and the event layer)

With those two files in place, the **pristine cs-wimp.h** compiles
unchanged: its `#if COMPILING_WIMPLIB` dance includes `./wimp.h` (the
ACW lineage), the `#else` branch includes `../tboxlibs/wimp.h` then
re-includes `./wimp.h` over it by defeating the guard.  Both lineages
coexist because they define different names (tboxlibs uses `Wimp*`,
rlib uses `wimp_*`); the guard defeat works because the tboxlibs header
doesn't share sub-includes with the rlib one.

**What went wrong the first time:** I tried to merge both lineages into
one header.  That required redefining every type the code uses --
BBox, event blocks, icon data, the poll block, 40+ message constants --
and each reinterpretation was a chance to get the layout wrong.  The
block dump proved it: the window-state block's word 0 didn't match the
DOCU field because my struct had the handle at a different offset than
what the wrapper wrote.  Meanwhile the two-lineage design in the
original code is not a bug to fix but a working architecture to serve.

## The rest of the port (unchanged from the first attempt)

### WimpLib's rlib sources: pinned to RISC_OSLib 5_82

The published repository does not ship the RISC_OSLib files that
`external/RISC_OSLib/rlib_patch/apply.sh` copies in and patches.  They
are provided from ROOL's `RISC_OSLib-5_82` tag: bbc, dbox, event,
fileicon, fontlist, fontselect, menu, res, resspr, win, wimp, wimpt,
xferrecv, xfersend.  flex from Toolboxlib's flexlib plus Stuart's
patch, ingested by cs-flex.c.

The `nd-*.c` units (werr, print, pointer, os, font, dboxtcol, drawmod)
ingest unpatched rlib sources; the box's rlib.a provides those members,
so they are not built.

### The assembler, in C

- `WimpLib/cs-poll.c` -- wimp_poll_coltsoft (hourglass off across the
  poll, FP-save bit, PollIdle for null-less callers)
- `WimpLib/cs-riscasm.c` -- EscH/EventH (the box's nested entries:
  `void entry(uint32_t regs[17])`, R11 the escape state), monotime,
  os_writeN/os_plot, hourglass and font veneers
- `cmodules/riscos/muldivas.c` -- muldiv64 natively

### The box shim (`WimpLib/cs-boxshim.c`)

zig clang's remapped libm (`__d_floor/__d_ceil/__d_trunc` by bits,
`__d_atan` by CORDIC), `__muldc3/__divdc3`, Report/RPCEmu debug SWI
veneers.  `target_riscos_host_box.h` = Stuart's clang target header
minus `USE_OWN_COMPLEX_IMPL` (the box's CLib has C99 complex.h).

### !Run (app packaging)

- `%*0` not bare `%*` (the box's kernel, like the ARM's, emits bare
  `%*` literally -- verified against Kernel/s/MoreSWIs)
- `PipeDream$Path` includes the WTemplates elements (bare-name lookups)
- `PipeDream$TemplatesPath` = the seed documents (Resources.UK.Templates.)
- WimpSlot -min 6656K

## The release build

The debug instruments the port was brought up with -- a serial crash
dump in place of the error dialogue, and a ring of the last sixteen
SWIs -- are out (issue #15): `cs-bbcx.c`, `cs-wimptx.c`, `cs-winx.c`,
`cursmov.c`, `savload.c` and `slotconv.c` are the 4.63 release's
again.  !Run checks for ColourPicker and DrawFile, as the stock !Run
does, with RMEnsure.

## How it was tested

On the Apple Silicon box (A64X32), driven as a user would drive it
(tests/lib/deskdrive: double-clicks, menus, keys, drags):

- a new document from the icon bar; the template dialogue; typing
  text and numbers; formulas, among them LN, LOG, ^, SQR, PI and the
  error strings;
- saving: a typed path, a drag to a Filer window, a drag to the Host
  icon;
- loading the PDExamples documents; charts drawn, a chart made from
  a selection, the pie gallery;
- the spelling checker finding and changing words;
- printing: a document printed to a PDF in PrintOut, text and
  figures where they belong.

Cross-check: a command file (`PipeDream:key`, run as PipeDream starts)
loads each of the 40 example documents and saves it as CSV, in this
port and in ARM PipeDream 4.62 run by the box's ARM container.  The
A64X32 and ARM sets, made in the same second, are identical.  The x32
set, made on the Intel box later, is identical but for the cells that
depend on the clock: NOW and its kin in Functions/Date, and RAND and
GRAND in Functions/Stats, whose generator PipeDream seeds from time()
when GRAND comes first.  The commands, one document to
two lines:

    \FL|i "HostFS::Host.$.Documents...doc" |i N |i "" |i N |i "" |i A |m
    \FS|i "HostFS::Host.$.Out.D00" |i N |i "" |i N |i "LF" |i C |m

and `\FX|m` to finish.

## Box-side findings from this port

Found while testing, 5 October 2026, and fixed in the box:

- the maths library lacked log, pow and the C99 entries PipeDream's
  functions call (clib, package L3b);
- `rawvdu:` and the other device filing systems did not exist; PipeDream
  prints its ruler through one (issue #136);
- text printed with Font_Paint came out blank: the Font Manager now
  passes Font_Paint to the printer driver while a page is drawn, as
  RISC OS's does (issue #160).

Found when the port was first brought up:

1. **DeskClock's sprite_redraw SIGSEGV** (sprite_redraw+0x50a) killed
   the box within 35 seconds; the VDU sprite redirection leak meant
   PipeDream's fonts painted into the clock's sprite.  DeskClock and
   DeskMeter are removed from the box build (Makefile, rom_contents.c,
   selftest.c).  Both are back in the ROM since, fixed; they start
   with the desktop when asked (`rosgd.deskclock`, `rosgd.deskmeter`).

2. **`rosgd.run=` kernel parameter** takes a single token (no spaces);
   multi-word commands need an Obey file on the share.

3. **Boxes started from a terminal die on stdin EOF** (~40s); run with
   stdin held open: `sleep 100000 | sh run/run-x86_64.sh`

## Open items

None.  Drag-to-folder, once an open item here, works.
