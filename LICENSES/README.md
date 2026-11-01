# Licences

*BOX does not claim ownership of any code that the project did not originally
write. The project is publishing translated source code: the C in `src/box` is
translated from other people's code, and keeps their licence. The project
actually builds most modules from the original assembler code, which is
typically more readable, and which is available from its own repository (RISC
OS Open's, at <https://gitlab.riscosopen.org/RiscOS/Sources>).*

The licence of everything the BOX releases are made from: the Mac releases
(`BOX-arm64-mac`, `BOX-x64-mac`) and the PC image (`BOX-x64-pc`), and of what
has been added to the BOX's sources since the last of them. The files were last
made from the sources of 1 November 2026; a release built earlier has earlier
versions of some of the same packages, under the same licences. A component
not yet in any release says so.

## The BOX itself

**The BOX's own code** is under the MIT licence ([box/BOX.txt](box/BOX.txt)).
That is any original code written for the BOX, and not translated from
anyone else's: the runtime, the modules and applications written new for it,
its tools (rosasm, rosbas, roscc), the compositor, and the interface to the
browser engine. The browser engine itself, WPE WebKit, is not the BOX's: it
is under its own licences (below).

**Translated code keeps the licence of the code it was translated from.**
When the BOX translates a program to C, the C is a translation of the
original work, not new work of the project's. It stays under the original
author's licence, and the MIT licence does not apply to it. The project
claims no ownership of any code it has translated, and the translation adds
no rights of the project's over it. Where the BOX's C is otherwise derived
from another author's source, that author's licence applies to it in the same
way.

**Ports keep their package's licence.** BOX's port of a package -- its
changes to it, and any C written to replace part of it -- is under that
package's licence, not MIT: PipeDream's under the MPL 2.0, NetSurf's under the
GNU GPL 2, StrongED's and StrongHelp's under the BSD licence, and so on.

**RISC OS** is RISC OS Open's, under the Apache License 2.0, with some
components under BSD-style licences ([box/RISCOS.txt](box/RISCOS.txt)).
Apache-licensed source is still Apache-licensed after it has been translated
to C: the licence itself counts a translation of the source as a form of the
work. So RISC OS translated to C is still RISC OS Open's, under the Apache
License 2.0, or under the BSD-style licence of the component it came from.
The same holds for every other program or library translated for the BOX.

## Publishing the source

It is the intention to publish all of the source code of this project. Each
port will be published when it is feature complete and maintainable. Publishing
incomplete, unreadable, automatically translated code would not be a useful
contribution to the community. The work will take several months.

Until then, the source of every component whose licence asks for it is
published here, in [../gpl](../gpl/README.md).

## Every component

One file each in [box/](box/), giving the version, where it comes from, what in
the BOX uses it, and then the licence as the package itself ships it. The same
files are on every BOX's disc, in `Documents.Licences`.

| Component | Version | Licence | In the BOX |
| --- | --- | --- | --- |
| [AcornFonts](box/AcornFonts.txt) | RISC OS 5.30 | Selwyn, Sidney: Apache License 2.0 (RISC OS Open's sources); the others: licence not known | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them (Selwyn and Sidney are also in the ROM's sources) |
| [AGG](box/AGG.txt) | 2.4 (as Haiku ships it) | AGG's own terms (permissive: the notice at the top of each of its files, which is all AGG 2.4 ships) | VectorIcons in /init, under Haiku's icon library: the rasteriser that draws a vector icon's paths (ports/vicon/haiku/agg). BOX's changes to it are under AGG's licence |
| [AWViewer](box/AWViewer.txt) | 2.18 (12-Apr-2018) | Freely distributable, unaltered and complete, free of charge (Computer Concepts and MW Software) | Apps.!AWViewer, as RISC OS 5.30's image has it, unaltered (ARM code, run by the ARM container); its modules render ArtWorks pictures for it and for OvationPro |
| [Boost](box/Boost.txt) | 1.89.0 | Boost Software License 1.0 | the ARM container in /init, which runs ARM code (runtime/armrun) (dynarmic is built with them) |
| [BOX](box/BOX.txt) | this disc's build | MIT | only the code written new for BOX, and translated from no one else's: the runtime (/init) and the modules written for it, the glue that joins the translated modules to it, rosbas, roscc, rosasm, pdfsvc, GraphTask, !Write's own code, the Mojo riscos package, and the disc's own applications and examples. Every package BOX ports, translates or builds on keeps its own licence, and so does BOX's port of it |
| [Cabin](box/Cabin.txt) | 2011 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [CADCraft](box/CADCraft.txt) | 0b32c2e6a1 | MIT or Apache License 2.0 | !FloorPlan's engine: its geometry, drawing and file formats (DXF; its color, doc, fonts, geom, io and render crates; not its interface) (on main since the releases; not yet in one) |
| [ChangeFSI](box/ChangeFSI.txt) | 1.70 (422a156c43) | Apache License 2.0 and New BSD, IJG for JPEG/, btpc's terms (PhotoCD is left out) | !ChangeFSI in the ROM's Apps, and the CFSI module |
| [Compositor](box/Compositor.txt) | 0.20.2 (tinywl), ROSGD 2026 | MIT (tinywl, wlroots) | the WPE edition: the screen put together, Linux programs' windows as Wimp windows |
| [craft-libs](box/craft-libs.txt) | 4 revisions (the file names them) | MIT or Apache License 2.0 | the ArtCraft libraries' shared crates, and so every engine built on them: the PDF back end and krilla, printing, the document path, colour management |
| [CraftFonts](box/CraftFonts.txt) | as the ArtCraft suite ships them | SIL Open Font License 1.1 | !Ledger's engine (the grid's text), !Draft's engine (a document's text, its code, and every label in a diagram) and !Publish's engine (DesignCraft carries its own copy of the same four faces), which have the faces built into them so that a document reads the same on every box |
| [curl](box/curl.txt) | 8.22.0 | curl licence (MIT-style) | /init (AcornHTTP and the URL fetcher) |
| [DeckCraft](box/DeckCraft.txt) | f34f4121c8 | MIT or Apache License 2.0 | !Present's engine: presentations (.pptx), their slides, their renderer and the PDF writer (its model, format, render, text and pptx crates; not its interface) |
| [DesignCraft](box/DesignCraft.txt) | 443ea4640c | MIT or Apache License 2.0 | !Publish's engine: page layout documents, their text composition and hyphenation, their renderer and the PDF, IDML and EPUB writers (its document, layout, tools, render and file-format crates; not its egui interface) (on main since the releases; not yet in one) |
| [dynarmic](box/dynarmic.txt) | a46601580d | 0BSD | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [Exo](box/Exo.txt) | 2011 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [Expat](box/Expat.txt) | 2.7.3 | MIT | !NetSurf (XML) |
| [Fanwood](box/Fanwood.txt) | 2011 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [fmt](box/fmt.txt) | 7b273fbb54 | MIT, with fmt's binary exception | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [FreeFont](box/FreeFont.txt) | 20120503 | GNU GPL 3 or later, with the font exception | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [GCC](box/GCC.txt) | 15.2.0 | GNU GPL 3 with the GCC Runtime Library Exception 3.1 | *Mojo: the compiler's C++ runtime (libstdc++ and libgcc), linked into it |
| [GeneralUser](box/GeneralUser.txt) | 2.0.3 | GeneralUser GS License v2.0 | Synth's General MIDI SoundFont, fetched by deps/get-soundfont.sh from the author's repository (github.com/mrbumpy409/GeneralUser-GS) and put on the disc by make usershare (on main since the releases; not yet in one) |
| [GLib](box/GLib.txt) | 2.88.3 | GNU LGPL 2.1 or later | ksmbd.tools |
| [gridcraft](box/gridcraft.txt) | 23829d8c67 | MIT or Apache License 2.0 | !Ledger's engine: spreadsheets (.xlsx, CSV), their calculation and their number formats |
| [Haiku](box/Haiku.txt) | Haiku's tree, taken 12 October 2026 | MIT | VectorIcons in /init: the renderer that draws vector icons (ports/vicon/haiku/icon) -- the HVIF reader, the icon model and IconRenderer; and tools' vicon. BOX's changes to it are under Haiku's licence |
| [IconArt](box/IconArt.txt) | the sets of 12 October 2026 | MIT (Haiku, Fluent); public domain (Tango); Apache License 2.0 (the Acorn art); SIL Open Font License 1.1 (the Noto Sans outlines) | Resources.Themes: the four vector icon sets (BOX, Win2K, Pinstripe and the Cube plinths), as VecIcons files loaded by *IconSprites. The composition and the tools are BOX's own; each source keeps its licence |
| [ksmbd-tools](box/ksmbd-tools.txt) | 3.5.7 | GNU GPL 2 or later | ksmbd.tools in the ROM (*Share, the box's SMB server) |
| [libjpeg-turbo](box/libjpeg-turbo.txt) | 3.2.0 | IJG licence, Modified (3-clause) BSD, zlib | /init (CompressJPEG, SpriteExtend's JPEG, ChangeFSI's CFSI), !NetSurf |
| [libnl](box/libnl.txt) | 3.12.0 | GNU LGPL 2.1 | ksmbd.tools |
| [libpng](box/libpng.txt) | 1.6.50 | libpng licence (PNG Reference Library License 2) | /init (CompressPNG, SpriteExtend's PNG, ChangeFSI's CFSI), !NetSurf |
| [libsmb2](box/libsmb2.txt) | commit fc710a3ebd | GNU LGPL 2.1 or later | LanManFS (*LMConnect), linked statically into /init |
| [Linux](box/Linux.txt) | 6.18.54 | GNU GPL 2, with the Linux syscall note | the kernel BOX runs on (both machines, and the PC image) |
| [LLVM](box/LLVM.txt) | 22.1.2 | Apache License 2.0 with LLVM Exceptions | both boxes' clang and ld.lld (*CC, *RosBas), compiler-rt's builtins; libc++, libc++abi and libunwind in /init (by zig) |
| [LuaJIT](box/LuaJIT.txt) | 2.1 (c6ffc14) | MIT | *LuaJIT, the Lua language, linked into /init |
| [lwIP](box/lwIP.txt) | 2.2.1 | BSD 3-clause | [HybrisOS](../HybrisOS/README.md), the HAL: its TCP/IP stack, compiled into the HAL edition's kernel image; not in the current releases |
| [Manuals](box/Manuals.txt) | RISC OS 5.30 | No licence stated; described in the manuals as public domain; distributed by RISC OS Open in RISC OS 5.30's image | Utilities.!Manuals, as RISC OS 5.30's image has it, unaltered: the manuals Assembly, Basic, FloatingPt, GPIO, InetSocket, InetSWIs, MiscSWIs, OS, SH-RefMan, Toolbox, VCache, VDU and Wimp |
| [mcl](box/mcl.txt) | 5fc4beaf33 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [Micropolis](box/Micropolis.txt) | 1e1e038, with BOX's changes | GNU GPL 3 with Electronic Arts' additional terms; the name under the Micropolis Public Name License | !Micropolis's engine (ports/rosapps/micropolis-engine): the city simulation and the pictures and sounds of the city (on main since the releases; not yet in one) |
| [mimalloc](box/mimalloc.txt) | 3.5.3 | MIT | both boxes' clang and the Mojo compiler (its memory allocator) |
| [Mojo](box/Mojo.txt) | 1.1 | Apache License 2.0 with LLVM Exceptions | *Mojo, BoxTools' Mojo compiler, and its standard library, under /usr/lib/mojo. The compiler is one static program, linked against musl with libstdc++, libgcc and mimalloc |
| [musl](box/musl.txt) | 1.2.6 | MIT | the C library of /init, ssh, sshd and ksmbd-tools (through zig), of the box's clang and the libraries it links with, of the box's POSIX C library, and the Mojo compiler (*Mojo), linked statically |
| [NetSurf](box/NetSurf.txt) | 3.11+ (39da3c3a40) | GNU GPL 2 (NetSurf); MIT (its libraries) | Apps.!NetSurf, the web browser, and the libraries it is built from: libwapcaplet, libparserutils, libhubbub, libdom, libcss, libnsgif, libnsbmp, libnsutils, libnspsl, libnslog, libsvgtiny. BOX's changes to NetSurf and its RISC OS front end are under NetSurf's licence |
| [oaknut](box/oaknut.txt) | 94c726ce03 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun), on Apple Silicon |
| [OpenSSH](box/OpenSSH.txt) | 10.5p1 | BSD-style (several) | ssh, ssh-keygen and sshd in the ROM (*SSH, logging in to the box) |
| [OpenSSL](box/OpenSSL.txt) | 3.6.4 | Apache License 2.0 | /init (AcornSSL, URL fetching), ssh and sshd |
| [OvationPro](box/OvationPro.txt) | 2.78 (26-July-2022) | Not stated in the application; distributed by RISC OS Open in RISC OS 5.30's image | Apps.!OvnPro, as RISC OS 5.30's image has it, unaltered (ARM code, run by the ARM container) |
| [Paige](box/Paige.txt) | 3.0b (ec53af9a25) | GNU LGPL 2.1 | !Write's text engine. BOX's changes to Paige are under Paige's licence |
| [PDFCraft](box/PDFCraft.txt) | ec0a965074 | MIT or Apache License 2.0 | !Folio's engine: PDF documents, their editing and their page renderer (hayro, vendored in it, is under the same licences) |
| [PhotoCraft](box/PhotoCraft.txt) | 03d0c7e3b3 | MIT or Apache License 2.0 | !Palette's and !Canvas's engines: pictures (PNG, JPEG), their compositing and their brushes |
| [PipeDream](box/PipeDream.txt) | 4.63 (b63a102bd6) | Mozilla Public License 2.0 | Apps.!PipeDream and Documents.PipeDream. BOX's port of PipeDream, its changes and the C that replaces its assembler, is under PipeDream's licence |
| [QEMU](box/QEMU.txt) | 11.1 (qemu-intel-mac) | GNU GPL 2; parts under other free licences | the Mac releases: BOX.app runs the box in it |
| [RiscLua](box/RiscLua.txt) | 8.6 | MIT | the riscos library of *LuaJIT, ported from RiscLua's riscoslib |
| [RISCOS](box/RISCOS.txt) | 5.30 (Kernel 6.70) | Apache License 2.0; some components BSD-style | the modules and applications translated to C for the box (the Kernel's SWIs, FileSwitch, the Wimp, the Font Manager and the ROM fonts Corpus, Homerton, Trinity, Selwyn and Sidney, Draw, ColourTrans, BASIC (BASICVFP), the Toolbox and its modules, DrawFile, MessageTrans, Territory, the networking modules and the rest); !Maestro and !CloseUp, and the Maestro tunes in Documents.Music, from RISC OS 5.30's HardDisc4 |
| [robin-map](box/robin-map.txt) | 054ec5ad67 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [Roboto](box/Roboto.txt) | 2011 | Apache License 2.0 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [Rust](box/Rust.txt) | 1.101.0-nightly 2026-09-30 | MIT or Apache License 2.0 | !PDF's module (rospdf), pdfsvc (the printer driver's PDF writer) and the ROSAPPS engines (!Scribe, !Trace, !Palette, !Ledger, !Folio, !Present, !Draft and !Workflows, !Code, !Studio, !Canvas, !Publish, !FloorPlan, !Micropolis, !Tower) |
| [RustCrates](box/RustCrates.txt) | Cargo.lock's | MIT, Apache 2.0, BSD, zlib, Unicode, Unlicense, per crate | !PDF (rospdf: hayro, the PDF interpreter, and vello_cpu, the rasteriser), pdfsvc (pdf-writer), and the ROSAPPS engines (!Scribe, !Trace, !Palette, !Ledger, !Folio, !Present, !Draft and !Workflows, !Code, !Studio, !Canvas, !Publish, !FloorPlan, !Micropolis, !Tower), which are built from the ArtCraft libraries, selkie, and the crates those use -- !Draft's painter draws with vello_cpu, the same rasteriser VectorCraft's renderer uses |
| [Selkie](box/Selkie.txt) | 0.3.0 | MIT | !Draft's diagram engine (ports/rosapps/diagram-engine): Mermaid's own languages -- flowchart, sequence, class, entity relationship, state, gantt, git, journey, timeline, C4 and architecture -- and the dagre layout that places a flowchart's nodes (on main since the releases; not yet in one) |
| [Simonetta](box/Simonetta.txt) | 2011-2012 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [SoundCraft](box/SoundCraft.txt) | 25d52c646a | MIT or Apache License 2.0 | !Studio's engine: the session, its tracks, the clips on them and the sound files behind those (its model, audio-io and time crates; not its engine crate, which carries plugin hosts the box has no use for) (on main since the releases; not yet in one) |
| [SparkFS](box/SparkFS.txt) | 1.50+ (git, 2026) | CDDL 1.0 and Apache License 2.0, file by file; SparkZip adds the Info-ZIP licence | !SparkFS in the ROM's Apps, its module and codecs |
| [SQLite](box/SQLite.txt) | 3.53.4 | public domain | Resources.Dictionaries.spelldict (an SQLite database) and the box's SQLite module (ROSSQLITE) |
| [StrongED](box/StrongED.txt) | 4.70a22 | 3-clause BSD | Apps.!StrongED: BOX's StrongED is new C, which uses StrongED's own data -- its syntax modes, help, messages, templates and key maps -- from the 4.70a22 release. BOX's StrongED is under StrongED's licence |
| [StrongHelp](box/StrongHelp.txt) | 2.90 (its data files) | BSD 3-clause | Resources:$.Apps.!StrongHlp (in the ROM), its data: its manual, Messages, Templates, menus, sprites, configuration and tools, as RISC OS 5.30's image has them, with !Run and !Boot changed where they used 2.90's module. The program is new C (design 31), written from StrongHelp's manuals, not from its source; it is under StrongHelp's licence |
| [SymSpell](box/SymSpell.txt) | 2025 | MIT | the spelling checker (ROSSPELL) and the word list behind Resources.Dictionaries.spelldict |
| [Themes](box/Themes.txt) | RISC OS 5.30 | as RISC OS Open distributes them in RISC OS 5.30 | Resources.Themes: the desktop themes (window furniture and icons) for *WimpTheme |
| [TinySoundFont](box/TinySoundFont.txt) | 853a0a1 (tsf.h) | MIT | Synth's General MIDI engine (modules/synth/gm.c), vendored in modules/synth/vendor (on main since the releases; not yet in one) |
| [TowerTogether](box/TowerTogether.txt) | 073c0e4, translated | MIT | !Tower's engine (ports/rosapps/tower-engine, over ports/tower/sim): the tower simulation, translated to Rust (on main since the releases; not yet in one) |
| [utf8proc](box/utf8proc.txt) | 2.12.0 | MIT, and the Unicode data licence | !NetSurf (Unicode) |
| [VectorCraft](box/VectorCraft.txt) | 9e7929ee89 | MIT or Apache License 2.0 | !Trace's engine: drawings (SVG, PostScript and EPS), their tools and their renderer |
| [WordCraft](box/WordCraft.txt) | 9b4b948c6e | MIT or Apache License 2.0 | !Scribe's engine: Word documents (.docx), their layout and their editing |
| [xbyak](box/xbyak.txt) | 8c0098b69f | 3-clause BSD | the ARM container in /init, which runs ARM code (runtime/armrun), on the Intel box |
| [Zig](box/Zig.txt) | 0.15.2 | MIT | the cross compiler /init and the Linux programs are built with; its compiler_rt is in them |
| [zlib](box/zlib.txt) | 1.3.2 | zlib licence | /init (the ZLib module, CompressPNG), !NetSurf |
| [Zycore](box/Zycore.txt) | 0b2432ced0 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun), on the Intel box |
| [Zydis](box/Zydis.txt) | bffbb610cf | MIT | the ARM container in /init, which runs ARM code (runtime/armrun), on the Intel box |

Not known: the licences of the fonts NewHall, System, Portrhouse and Sassoon
(AcornFonts), and of the picture `Docs.Apple`, whose origin is not recorded.

## The web browser's Linux userland

Since 7 October 2026 every release carries the WPE edition's Linux root: WPE
WebKit and what it needs, as Debian's own packages (Debian forky), unchanged,
with the BOX's compositor and its interface to the browser engine added. It is a disk image
inside BOX.app (`Contents/Resources/system/wpe-root.ext4`) and a partition of
the PC image.

* [wpe/packages-arm64.txt](wpe/packages-arm64.txt) -- the Apple silicon root:
  196 packages, each with its version and the Debian source package it is built
  from.
* [wpe/packages-amd64.txt](wpe/packages-amd64.txt) -- the Intel Mac's and the
  PC's: 199 packages.
* [wpe/copyright/](wpe/copyright/) -- each package's licence, as Debian ships it
  (`/usr/share/doc/<package>/copyright` in the root). The chief ones:
  [WPE WebKit](wpe/copyright/libwpewebkit-2.0-1.txt) (BSD 2-clause, GNU LGPL
  2.1 or later, MPL 2.0 and others, file by file),
  [GLib](wpe/copyright/libglib2.0-0t64.txt) (GNU LGPL 2.1 or later, and others),
  [Mesa](wpe/copyright/libgl1-mesa-dri.txt) (MIT and others),
  [wlroots](wpe/copyright/libwlroots-0.20.txt) (MIT, and others),
  [bubblewrap](wpe/copyright/bubblewrap.txt) (GNU LGPL 2 or later),
  [glibc](wpe/copyright/libc6.txt) (GNU LGPL 2.1 or later, and others). Each
  file says which licence covers which part.

## Source code

* **Debian's packages**: each at its version in Debian's archive,
  `https://snapshot.debian.org/package/<source>/<version>/` (the source column
  of the package lists).
* **Linux** 6.18.54: unchanged, from [kernel.org](https://www.kernel.org/).
* **QEMU**, the build in the Mac releases:
  [github.com/albanread/qemu-intel-mac](https://github.com/albanread/qemu-intel-mac).
* **RISC OS**: RISC OS Open's sources,
  [riscosopen.org](https://www.riscosopen.org/content/downloads).
* The other components: where each file in [box/](box/) says it comes from.
* **The copyleft components as the BOX builds them** -- its ports, patches,
  build scripts and kernel configurations: [../gpl](../gpl/README.md).
