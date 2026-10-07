# Licences

The licence of everything the BOX releases are made from: the Mac releases
(`BOX-arm64-mac`, `BOX-x64-mac`) and the PC image (`BOX-x64-pc`), as of the
releases of 7 October 2026.

## The BOX itself

The BOX's own code -- the translation of RISC OS to C, the runtime, the
native modules, the compositor and the browser's engine -- is under the MIT
licence ([box/BOX.txt](box/BOX.txt)).

**RISC OS** is RISC OS Open's, under the Apache License 2.0, some components
BSD-style ([box/RISCOS.txt](box/RISCOS.txt)). The BOX translates it; no
ownership is claimed over any work translated to C, and every component keeps
its upstream terms.

## Every component

One file each in [box/](box/), giving the version, where it comes from, what in
the BOX uses it, and then the licence as the package itself ships it. The same
files are on every BOX's disc, in `Documents.Licences`.

| Component | Version | Licence | In the BOX |
| --- | --- | --- | --- |
| [AcornFonts](box/AcornFonts.txt) | RISC OS 5.30 | Selwyn, Sidney: Apache License 2.0 (RISC OS Open's sources); the others: licence not known | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them (Selwyn and Sidney are also in the ROM's sources) |
| [AWViewer](box/AWViewer.txt) | 2.18 (12-Apr-2018) | Freely distributable, unaltered and complete, free of charge (Computer Concepts and MW Software) | Apps.!AWViewer, as RISC OS 5.30's image has it, unaltered (ARM code, run by the ARM container); its modules render ArtWorks pictures for it and for OvationPro |
| [Boost](box/Boost.txt) | 1.89.0 | Boost Software License 1.0 | the ARM container in /init, which runs ARM code (runtime/armrun) (dynarmic is built with them) |
| [BOX](box/BOX.txt) | this disc's build | MIT | everything of BOX's own: the runtime (/init), the translated modules' glue, rosbas, roscc, rosasm, pdfsvc, GraphTask, !Write, the StrongED, PipeDream and NetSurf front ends, the Mojo riscos package, the disc's own applications and examples |
| [Cabin](box/Cabin.txt) | 2011 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [ChangeFSI](box/ChangeFSI.txt) | 1.70 (422a156c43) | Apache License 2.0 and New BSD, IJG for JPEG/, btpc's terms (PhotoCD is left out) | !ChangeFSI in the ROM's Apps, and the CFSI module |
| [Compositor](box/Compositor.txt) | 0.20.2 (tinywl), ROSGD 2026 | MIT (tinywl, wlroots) | the WPE edition: the screen put together, Linux programs' windows as Wimp windows |
| [curl](box/curl.txt) | 8.22.0 | curl licence (MIT-style) | /init (AcornHTTP and the URL fetcher) |
| [dynarmic](box/dynarmic.txt) | a46601580d | 0BSD | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [Exo](box/Exo.txt) | 2011 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [Expat](box/Expat.txt) | 2.7.3 | MIT | !NetSurf (XML) |
| [Fanwood](box/Fanwood.txt) | 2011 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [fmt](box/fmt.txt) | 7b273fbb54 | MIT, with fmt's binary exception | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [FreeFont](box/FreeFont.txt) | 20120503 | GNU GPL 3 or later, with the font exception | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [GCC](box/GCC.txt) | 14.2.0-19 | GNU GPL 3 with the GCC Runtime Library Exception 3.1 | *Mojo: the compiler's C++ runtime, under /usr/lib/mojo/lib |
| [GLib](box/GLib.txt) | 2.88.3 | GNU LGPL 2.1 or later | ksmbd.tools |
| [glibc](box/glibc.txt) | 2.41-12+deb13u4 | GNU LGPL 2.1 or later (some parts under other free licences: LICENSES) | *Mojo: the compiler runs on its own copy of glibc, under /usr/lib/mojo/lib |
| [ksmbd-tools](box/ksmbd-tools.txt) | 3.5.7 | GNU GPL 2 or later | ksmbd.tools in the ROM (*Share, the box's SMB server) |
| [libjpeg-turbo](box/libjpeg-turbo.txt) | 3.2.0 | IJG licence, Modified (3-clause) BSD, zlib | /init (CompressJPEG, SpriteExtend's JPEG, ChangeFSI's CFSI), !NetSurf |
| [libnl](box/libnl.txt) | 3.12.0 | GNU LGPL 2.1 | ksmbd.tools |
| [libpng](box/libpng.txt) | 1.6.50 | libpng licence (PNG Reference Library License 2) | /init (CompressPNG, SpriteExtend's PNG, ChangeFSI's CFSI), !NetSurf |
| [Linux](box/Linux.txt) | 6.18.54 | GNU GPL 2, with the Linux syscall note | the kernel BOX runs on (both machines, and the PC image) |
| [LLVM](box/LLVM.txt) | 22.1.2 | Apache License 2.0 with LLVM Exceptions | both boxes' clang and ld.lld (*CC, *RosBas), compiler-rt's builtins; libc++, libc++abi and libunwind in /init (by zig) |
| [Manuals](box/Manuals.txt) | RISC OS 5.30 | No licence stated; described in the manuals as public domain; distributed by RISC OS Open in RISC OS 5.30's image | Utilities.!Manuals, as RISC OS 5.30's image has it, unaltered: the manuals Assembly, Basic, FloatingPt, GPIO, InetSocket, InetSWIs, MiscSWIs, OS, SH-RefMan, Toolbox, VCache, VDU and Wimp |
| [mcl](box/mcl.txt) | 5fc4beaf33 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [mimalloc](box/mimalloc.txt) | 3.5.3 | MIT | both boxes' clang (its memory allocator) |
| [Mojo](box/Mojo.txt) | 1.1 | Apache License 2.0 with LLVM Exceptions | *Mojo, BoxTools' Mojo compiler, and its standard library, under /usr/lib/mojo |
| [musl](box/musl.txt) | 1.2.6 | MIT | the C library of /init, ssh, sshd and ksmbd-tools (through zig), of the box's clang and the libraries it links with, and of the box's POSIX C library |
| [NetSurf](box/NetSurf.txt) | 3.11+ (39da3c3a40) | GNU GPL 2 (NetSurf); MIT (its libraries) | Apps.!NetSurf, the web browser, and the libraries it is built from: libwapcaplet, libparserutils, libhubbub, libdom, libcss, libnsgif, libnsbmp, libnsutils, libnspsl, libnslog, libsvgtiny |
| [oaknut](box/oaknut.txt) | 94c726ce03 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun), on Apple Silicon |
| [OpenSSH](box/OpenSSH.txt) | 10.5p1 | BSD-style (several) | ssh, ssh-keygen and sshd in the ROM (*SSH, logging in to the box) |
| [OpenSSL](box/OpenSSL.txt) | 3.6.4 | Apache License 2.0 | /init (AcornSSL, URL fetching), ssh and sshd |
| [OvationPro](box/OvationPro.txt) | 2.78 (26-July-2022) | Not stated in the application; distributed by RISC OS Open in RISC OS 5.30's image | Apps.!OvnPro, as RISC OS 5.30's image has it, unaltered (ARM code, run by the ARM container) |
| [Paige](box/Paige.txt) | 3.0b (ec53af9a25) | GNU LGPL 2.1 | !Write's text engine |
| [PipeDream](box/PipeDream.txt) | 4.63 (b63a102bd6) | Mozilla Public License 2.0 | Apps.!PipeDream and Documents.PipeDream |
| [QEMU](box/QEMU.txt) | 11.1 (qemu-intel-mac) | GNU GPL 2; parts under other free licences | the Mac releases: BOX.app runs the box in it |
| [RISCOS](box/RISCOS.txt) | 5.30 (Kernel 6.70) | Apache License 2.0; some components BSD-style | the modules and applications translated to C for the box (the Kernel's SWIs, FileSwitch, the Wimp, the Font Manager and the ROM fonts Corpus, Homerton, Trinity, Selwyn and Sidney, Draw, ColourTrans, BASIC (BASICVFP), the Toolbox and its modules, DrawFile, MessageTrans, Territory, the networking modules and the rest); !Maestro and !CloseUp, and the Maestro tunes in Documents.Music, from RISC OS 5.30's HardDisc4 |
| [robin-map](box/robin-map.txt) | 054ec5ad67 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun) |
| [Roboto](box/Roboto.txt) | 2011 | Apache License 2.0 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [Rust](box/Rust.txt) | 1.101.0-nightly 2026-09-30 | MIT or Apache License 2.0 | !PDF's module (rospdf) and pdfsvc (the printer driver's PDF writer) |
| [RustCrates](box/RustCrates.txt) | Cargo.lock's | MIT, Apache 2.0, BSD, zlib, Unicode, Unlicense, per crate | !PDF (rospdf: hayro, the PDF interpreter, and vello_cpu, the rasteriser) and pdfsvc (pdf-writer) |
| [Simonetta](box/Simonetta.txt) | 2011-2012 | SIL Open Font License 1.1 | RISC OS 5.30's !Fonts (Resources.!Fonts on the disc), as RISC OS Open converted them |
| [SparkFS](box/SparkFS.txt) | 1.50+ (git, 2026) | CDDL 1.0 and Apache License 2.0, file by file; SparkZip adds the Info-ZIP licence | !SparkFS in the ROM's Apps, its module and codecs |
| [SQLite](box/SQLite.txt) | 3.53.4 | public domain | Resources.Dictionaries.spelldict (an SQLite database) and the box's SQLite module (ROSSQLITE) |
| [StrongED](box/StrongED.txt) | 4.70a22 | 3-clause BSD | Apps.!StrongED: BOX's StrongED is new C, which uses StrongED's own data -- its syntax modes, help, messages, templates and key maps -- from the 4.70a22 release |
| [StrongHelp](box/StrongHelp.txt) | 2.90 (its data files) | BSD 3-clause | Resources:$.Apps.!StrongHlp (in the ROM), its data: its manual, Messages, Templates, menus, sprites, configuration and tools, as RISC OS 5.30's image has them, with !Run and !Boot changed where they used 2.90's module. The program is BOX's own C (design 31; the file BOX), written from StrongHelp's manuals, not from its source |
| [SymSpell](box/SymSpell.txt) | 2025 | MIT | the spelling checker (ROSSPELL) and the word list behind Resources.Dictionaries.spelldict |
| [utf8proc](box/utf8proc.txt) | 2.12.0 | MIT, and the Unicode data licence | !NetSurf (Unicode) |
| [xbyak](box/xbyak.txt) | 8c0098b69f | 3-clause BSD | the ARM container in /init, which runs ARM code (runtime/armrun), on the Intel box |
| [Zig](box/Zig.txt) | 0.17.0 | MIT | the cross compiler /init and the Linux programs are built with; its compiler_rt is in them |
| [zlib](box/zlib.txt) | 1.3.2 | zlib licence | /init (the ZLib module, CompressPNG), !NetSurf |
| [Zycore](box/Zycore.txt) | 0b2432ced0 | MIT | the ARM container in /init, which runs ARM code (runtime/armrun), on the Intel box |
| [Zydis](box/Zydis.txt) | bffbb610cf | MIT | the ARM container in /init, which runs ARM code (runtime/armrun), on the Intel box |

Not known: the licences of the fonts NewHall, System, Portrhouse and Sassoon
(AcornFonts), and of the picture `Docs.Apple`, whose origin is not recorded.

## The web browser's Linux userland

Since 7 October 2026 every release carries the WPE edition's Linux root: WPE
WebKit and what it needs, as Debian's own packages (Debian forky), unchanged,
with the BOX's compositor and the browser's engine added. It is a disk image
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
