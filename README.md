# BOX

A RISC OS environment for generic devices.

*RISC OS's components translated to C, running on a Linux kernel, on a 64-bit PC or a Mac.*

**This is not RISC OS.** BOX is a RISC OS window manager and application
environment running on a Linux kernel. It is not RISC OS, it is not a port
of RISC OS, and it does not claim to be either. It aims to be reasonably
compatible: it runs translated applications, and some emulated ones.

![BOX on a PC, booted from a USB stick](screenshots/box-on-a-pc.png)

## Sunday 18th October release, for the Mac

The two Mac releases. The PC release is still the 11th's, below.

### Release notes

New features:

- **The Mac composites the desktop.** The Mac's window now draws each of the desktop's windows itself, from the box's memory, rather than a copy of the whole screen. !Browser shows its pages with the Mac's own WebKit.
- **The new BOX apps.** !Scribe, !Trace, !Palette, !Ledger, !Folio and !Present, in Apps on the Host disc; see [Apps](#apps).
- **`*Present FILE`** opens a presentation and shows it on the whole screen at once. Documents.BOX holds one about BOX.
- **A new disc** starts with the Iyonix theme, RISC OS 5.30's own default, and a plain grey desktop.
- **The network is set up in the box,** and the boot no longer waits for it.

Fixes (2026.10.18.4):

- GraphTask shows every frame, and the desktop keeps running while a program in it waits for the screen. Before, a program that waited each frame held the whole desktop for as long as it ran.
- A stroke in !Palette no longer runs on to the next click.

### Downloads

| Machine | Download | Needs |
| --- | --- | --- |
| Apple silicon Mac | [BOX-arm64-mac-2026.10.18](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026.10.18) | macOS 14 or later; M1 or later |
| Intel Mac | [BOX-x64-mac-2026.10.18](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-mac-2026.10.18) | macOS 14 or later |
| Intel PC, UEFI boot | [BOX-x64-pc-2026.10.11](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-pc-2026.10.11) (the 11th's) | a 64-bit PC that boots by UEFI with Secure Boot off; a USB stick of 16 GB or more |

To update, drop the new BOX over the old one. BOX brings the programs on your disc up to date the first time it runs, and leaves what you saved alone.

## Work in progress, Sunday 18th October 2026

Not released yet on the PC. What has been going on since the 11th:

- **Compositing.** On the PC builds every window now has a picture of its own, and a separate program puts the desktop together on the graphics card, with shadows and rounded corners over a backdrop the card draws itself. See [The Compositing Wimp](articles/8-Compositing.md).
- **Four PCs tested.** The same build has been run on an i7-12700 with an NVIDIA T1000, an Alder Lake-N mini PC, a Dell Latitude laptop, and a PC booted from a USB stick. See [BOX on a Low-Cost Mini PC](articles/9-MiniPC.md).
- **New BOX RISC OS apps.** A word processor, a drawing program, a paint program, a spreadsheet, a PDF editor and a presentation program, written for BOX, which open the document formats the rest of the world uses. See [Apps for BOX](articles/10-Apps.md). In the Mac release of the 18th; not yet on the PC.
- **PC BOX: video acceleration for YouTube, GL acceleration for WebGL.** The browser decodes video on the graphics part and runs WebGL on it, so a mini PC with integrated graphics plays YouTube and holds sixty frames a second in a WebGL page.

## Sunday 11th October release

The same release for all three kinds of machine, built from one source.

### Release notes

New features:

- **Lua.** LuaJIT, with `*LuaJIT` to run Lua scripts and a `riscos` library for RISC OS calls.
- **Painted desktop themes.** The desktop can be drawn in themes, from painted sprites.
- **The Appearance application,** to choose the theme and the desktop's look.
- **Metal acceleration on the Mac.** The Mac releases draw the screen through Metal.
- **New metrics views** from the monitor (DeskMeter): a Linux window showing each processor, memory, paging, system calls, SWIs, disc and network a second, and the `*LinuxStats` command.

Also:

- Reading the clock no longer costs a system call on Apple silicon, so SWIs run about three times faster. Intel Macs and PCs always read it this way, so this brings Apple silicon up to the same speed rather than making it quicker than Intel.
- Miscellaneous bug fixes and quality improvements.

Fixes (2026.10.11.3):

- The mouse pointer no longer disappears after the desktop theme is changed.
- Sound from several programs now plays together, and SharedSound's handlers run safely.

### Downloads

| Machine | Download | Needs |
| --- | --- | --- |
| Apple silicon Mac | [BOX-arm64-mac-2026.10.11](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026.10.11) | macOS 14 or later; M1 or later |
| Intel Mac | [BOX-x64-mac-2026.10.11](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-mac-2026.10.11) | macOS 14 or later |
| Intel PC, UEFI boot | [BOX-x64-pc-2026.10.11](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-pc-2026.10.11) | a 64-bit PC that boots by UEFI with Secure Boot off; a USB stick of 16 GB or more |

## Other downloads

BBC BASIC on its own, and every earlier preview, are on the
[Releases page](https://github.com/albanread/Aldershot/releases):

- [Just BBC BASIC translated](https://github.com/albanread/Aldershot/releases/tag/BBCBASICVA64-1.2) (BBCBASICVA64, Apple silicon Macs, macOS 14 or later)
- [Just BBC BASIC translated, for Intel Macs](https://github.com/albanread/Aldershot/releases/tag/BBCBASICVX64-1.2) (BBCBASICVX64, macOS 14 or later)

## Apps

Apps for RISC OS BOX, written in easy to understand Mojo, backed by Rust library code. All of them are work in progress.

A RISC OS desktop should be able to open the files everyone else sends: a `.docx`, an SVG, a PDF, an `.xlsx`. So BOX has applications of its own, split in two. The part the person uses is an ordinary Wimp task, written in Mojo, which behaves as a RISC OS application should. The part that understands the document is a Rust engine, a separate Linux program, built on large Rust libraries written in the open, of which we keep our own copy, with our own code for each kind of document over them.

Documents:

- [!Scribe](src/box/apps/scribe/) is a word processor. It opens and saves Word documents (`.docx`, and opens the older `.doc`), and exports PDF, Rich Text, OpenDocument, text and HTML.
- [!Ledger](src/box/apps/ledger/) is a spreadsheet. It opens and saves Excel workbooks (`.xlsx`) and CSV files.
- [!Folio](src/box/apps/folio/) is a PDF reader and editor. It adds notes, highlights and ink, and moves and turns pages.
- [!Present](src/box/apps/present/) is a presentation program. It opens and saves PowerPoint files (`.pptx`) and shows them full screen.
- [!Publish](src/box/apps/publish/) lays out pages, with stories that run from frame to frame, and exports PDF, IDML and EPUB.
- [!Draft](src/box/apps/draft/) writes Markdown with diagrams in it, drawn as a page.

Pictures:

- [!Trace](src/box/apps/trace/) is a drawing program. It opens and saves SVG, opens PostScript, EPS, DXF and Illustrator files, imports Draw and ArtWorks files, and exports Draw, ArtWorks, PDF and pictures.
- [!Palette](src/box/apps/palette/) is a photo editor. It opens PNG, JPEG, TIFF, GIF and BMP pictures, saves PNG, and exports sprites and the other formats.
- [!Canvas](src/box/apps/canvas/) is a painting program, with brushes that behave like paint.
- [!FloorPlan](src/box/apps/floorplan/) draws plans of houses and small offices.

Sound, programs and work:

- [!Studio](src/box/apps/studio/) is a multitrack studio for WAV, AIFF and FLAC sounds.
- [!Code](src/box/apps/code/) is the programmer's editor, with language servers, Build and Run, and a debugger.
- [!Workflows](src/box/apps/workflows/) draws Tcl workflows: the steps a document goes through.

Games: [!Micropolis](src/box/apps/micropolis/), the city simulation; [!Tower](src/box/apps/tower/), a tower-building game; and !Antiquity, a 3D strategy game, still in development.

All of them are one program, the [shell](src/box/apps/shell/), with a configuration each; !Shell is the shell with an engine that does nothing.

The Mojo source of the shell and of every application but !Antiquity is in [src/box/apps](src/box/apps/). The engines are not published there; those of !Micropolis and !Tower are published with their simulations, in [box-micropolis](https://github.com/albanread/box-micropolis) and [box-tower](https://github.com/albanread/box-tower).

[Apps for BOX](articles/10-Apps.md) describes how they are put together, why Mojo for the user interface and Rust underneath it, and how the two halves talk.

## Ports

The modules and applications in BOX are ported. Some need only small changes. Others are rewritten completely in C. The work is done a step at a time, and a port may bring in bugs that the original does not have.

Please do not report missing features or faults in BOX's software to the original authors. Their ARM code very likely works perfectly on a Raspberry Pi 4; the fault is in the port, not in their work.

There is no need to report them here either. The project finds faults itself, by testing BOX against RISC OS 5.30.

## Performance

RISC OS's own ARM code is extremely efficient on an ARM processor such as the Raspberry Pi 4's Cortex-A72. Porting that code to C costs a good deal of speed, and needs more memory. Translated C is also 1.5 to 3 times slower than C written by hand.

So BOX is not suited to Raspberry Pi class machines. It performs well on computers such as Apple silicon Macs, or PCs with an Intel Core i7-12700, or even a low cost mini PC: see [BOX on a Low-Cost Mini PC](articles/9-MiniPC.md).

BBC BASIC suffers from the translation too, and does not run as much faster as the difference in processor power might suggest. To make up for this, BOX includes a translator from BBC BASIC to C, `*RosBas`.

## Documents

- [articles](articles/README.md): technical articles about BOX, in the style of the
  RISC OS Programmer's Reference Manuals. Start with [BOX Design](articles/0-Design.md).
- [docs](docs/README.md): documents about BOX. [Bugs and quirks](docs/bugs-and-quirks.md) lists the bugs, quirks and documentation errors noticed in RISC OS 5.30 and its applications while porting them.

## Licences

Important licence update information has been added to the repository: see [LICENSES](LICENSES/README.md).

*Anyone with genuine concerns about software licences should raise an issue.*

*BOX does not claim ownership of any code that the project did not originally write. The project is publishing translated source code: the C in [src/box](src/box/README.md) is translated from other people's code, and keeps their licence. The project actually builds most modules from the original assembler code, which is typically more readable, and which is available from its own repository ([RISC OS Open's](https://gitlab.riscosopen.org/RiscOS/Sources)).*

- [LICENSES](LICENSES/README.md): the licence of every component. BOX's own code is under the MIT licence. RISC OS is RISC OS Open Ltd's, under the Apache License 2.0.
- [gpl](gpl/README.md): the source of the components under the GNU GPL, GNU LGPL, MPL 2.0 and CDDL.

Code that BOX has translated to C stays under the licence of the code it was translated from: RISC OS translated to C is still under the Apache License 2.0. BOX claims no ownership of translated code.

## Source code

The project is publishing translated source code: the C in [src/box](src/box/README.md) is translated from other people's code, and keeps their licence. The project actually builds most modules from the original assembler code, which is typically more readable, and which is available from its own repository ([RISC OS Open's](https://gitlab.riscosopen.org/RiscOS/Sources)).

The source of every component whose licence asks for it is in [gpl](gpl/README.md).

Published so far, in [src](src):

- [BOX](src/box/README.md), the source of `/init`: the runtime, the platform layer, the boot code and the modules written for BOX;
- [ROSASM](src/ROSASM/README.md), the ObjAsm-compatible assembler;
- [ROSBAS](src/ROSBAS/README.md), the BBC BASIC V compiler (the compiler only; its runtime library will follow).
- [BBC BASIC V, translated to C](src/translated/bbcbasic/README.md): RISC OS's BBC BASIC interpreter, translated by hand from ARM assembler; under ROOL's Apache License 2.0.
- [The Compositing Wimp](src/compositing-wimp/README.md): the Window Manager as the PC builds run it, where every window has a picture of its own, with the presenter that composites the desktop and the block format they share; under ROOL's Apache License 2.0, with the BOX project's compositing work.
