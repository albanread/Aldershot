# The PipeDream port

PipeDream 4.63 -- Colton Software's spreadsheet / word processor /
database / charts program, open-sourced by Stuart Swales under
MPL-2.0 -- built for the box as an x32 module image: clang compiling
the original C through roscc, linked against rlib.a and the ROM C
library.  Status: **working**, x32 and A64X32, release build (no
debug instruments).  Tested on the Apple Silicon box as a user would
use it, by mouse and keyboard: new documents and templates, typing,
formulas, saving (typed, to a Filer window and to the Host icon),
loading the examples, charts (made and drawn), the spelling checker
and printing.  The examples' results agree with ARM PipeDream 4.62's,
on both boxes (PORT_NOTES.md, "How it was tested").

    sh tools/build.sh all       # compile (148 units) and link: rosgd/build/pipedream/!RunImage
    ARCH=aarch64 sh tools/build.sh all   # A64X32, for the Apple Silicon box:
                                         # rosgd/build/aarch64/pipedream/!RunImage

The shipped application is `disc/Apps/!PipeDream`: this port's
!RunImage on the 4.62 install's Resources (the 4.63 release tarball
carries none), with !Run/!Boot adapted -- the stock ones' RMEnsure
version gates and Loader startup dropped, the Choices seeding kept
(see that !Run's comments for the two OS_CLI behaviours it works
around).

[PORT_NOTES.md](PORT_NOTES.md) is the port's own record, written as
the work happened -- above all the lesson of the first, failed
attempt: be minimal.  The original code compiles through two
coexisting Wimp header lineages (rlib's and ToolboxLib's); the port
reconstructs both from real files instead of merging them, and
changes three things from pristine, plus one fault fixed (a NULL
compare in printing).  The notes are worth reading
before porting any C application of the same vintage.

The sources are the 4.63 release (tag `pipedream__2025_10_29__4_63_00`)
with the reconstructed headers and pinned RISC_OSLib 5.82 rlib members
the build needs; the licence is [LICENSE](LICENSE) (MPL-2.0,
Copyright (c) 2013-2023 Stuart Swales).
