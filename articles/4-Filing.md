# HostFS and the Filing Systems

*8 October 2026*

## Introduction

On a Raspberry Pi, RISC OS keeps its files on Acorn's own filing
systems: FileCore discs on an SD card or a USB drive, reached through
SDFS or SCSIFS. BOX has none of these. It keeps its files on Linux's
filing systems, and shows them to RISC OS through HostFS.

That is the largest single departure BOX makes from RISC OS, so it is
worth explaining properly rather than in passing. This article covers:

* why BOX does not implement FileCore, and why that is a decision taken
  out of care rather than preference
* FileSwitch, which BOX rewrote, and what sits under it
* HostFS, and how it maps Linux files to RISC OS files
* ResourceFS, which holds the ROM's own files
* SparkFS, the compressed filing system
* booting and running RISC OS from a USB disc.

---

## 1. Why there are no Acorn filing systems

BOX does not implement FileCore, or any of the filing systems built on
it — ADFS, SCSIFS, SDFS and RamFS — nor CDFS.

This is not a judgement on that code. FileCore is careful work that has
looked after people's data for more than thirty years, and the format it
maintains is one that users still hold archives in. It is precisely
because of that, and not in spite of it, that BOX leaves it alone.

### A filing system is not like other software

Most of BOX is converted from ARM assembler to C, by machine translation
or by rewriting from a specification (see *Tiers of Translation*). Both
routes produce working code, and both have been used on substantial
parts of the system. Neither is a comfortable way to arrive at a filing
system.

The reason is the consequence of being wrong. If the Window Manager has
a bug, a window is drawn in the wrong place, and you restart the
machine. If a filing system has a bug, it writes the wrong block to a
disc. The damage is silent, it is permanent, it may not be noticed for
weeks, and the volume it damages may hold the only copy of something.
There is no restart that undoes it.

### Where the particular risk lies

Two properties of this work make it a poor candidate for either
conversion route.

**It is sensitive to word size.** A disc format is a contract about
exact widths: 32-bit block numbers, 32-bit offsets, structures laid out
to the byte, arithmetic that is expected to wrap or to be signed in a
particular way. BOX compiles to 64-bit host code. In most of the system
a value that becomes wider than it was is harmless — a sign extension in
a renderer draws the same pixels. In directory arithmetic, the same
difference computes a block number that is off, and the write goes to
the wrong place on a real disc. These are exactly the mistakes that are
easiest to make and hardest to see, because the code runs, the test
passes, and the corruption appears later.

**Approximately right is not a category.** A rewrite from a
specification is only ever as good as the specification, and BOX has
already met that limit in a gentler setting: the native Window Manager
was written from a thirteen-chapter specification and testing still
found places where the specification was wrong. That is an acceptable
way to arrive at a window manager, where the farm shows you the
difference and you fix it. It is not an acceptable way to arrive at the
code that decides where your files go.

### BOX is experimental, which argues for more care, not less

It would be easy to reason the other way — that an experimental system
may take liberties because nobody is relying on it yet. We think that is
backwards. People trying BOX will reasonably point it at a disc that has
their work on it. A system still finding its feet is the last thing that
should be writing to a volume by rules it has only partly reimplemented.

### What BOX does instead

Linux already manages discs, for far more kinds of device than RISC OS
ever supported, with filing systems that are exercised continuously by a
very large number of users and repaired by people who do nothing else.
Putting a second, newly converted filing system on top of a disc that
Linux can already read and write would add risk and subtract nothing.

So BOX uses Linux's filing systems, and HostFS presents them to RISC OS.
FileCore discs are left to the systems that implement FileCore properly:
real hardware, and the emulators.

### What it costs you

Plainly, so that nobody is surprised: **a FileCore disc or disc image
cannot be read in BOX.** If you have one, copy the files off it first on
a machine that can read it — real hardware, or an emulator — and then
work with them in BOX.

That is a real limitation and we would rather state it than dress it up.
It is the price of not being casual with data.

---

## 2. FileSwitch

FileSwitch is the module that owns RISC OS's file SWIs: `OS_File`,
`OS_Find`, `OS_Args`, `OS_GBPB`, `OS_BGet`, `OS_BPut` and
`OS_FSControl`. It parses names, handles path variables and wildcards,
keeps track of open files, and passes each operation to the filing
system concerned.

BOX's FileSwitch is a rewrite in C, made from FileSwitch's own sources
and the PRM. It keeps RISC OS's rules: the same names, the same errors
and error numbers, the same stream behaviour, and the same UpCalls to
tell the Filer that a file has changed, so that Filer windows update
themselves.

Note what FileSwitch is and is not. It is the part that understands
*RISC OS's* conventions — names, types, paths, handles — and none of it
decides where a byte lands on a disc. That is the half it is safe and
sensible to reimplement, and it is the half BOX did.

Under it are its own filing systems (HostFS, ResourceFS, and LanMan for
SMB shares), and any filing system or image filing system a module adds
in the usual way through `OS_FSControl`.

---

## 3. HostFS

HostFS is the filing system for files that Linux holds. Each Linux mount
it is given is one HostFS disc:

| Disc | What it is |
| --- | --- |
| `Host` | on a Mac, the host's own folder, shared into the box |
| `Disc` | the RISC OS disc: the ext4 partition of the USB stick on a PC |
| others | SMB shares from Windows, Macs or Linux, mounted by `*LMConnect` |

`HostFS::Host.$.Apps` is the `Apps` directory of the shared folder, and
`HostFS:$` is the first disc. Whatever Linux can mount becomes a RISC OS
disc in the same way.

### Mapping names, types and attributes

HostFS follows the conventions already used by the team's HostFS for
emulators, so the same folder reads the same way in either:

* **Names.** A RISC OS `/` is a Linux `.`, so `ReadMe/txt` is
  `ReadMe.txt` on the host. Latin-1 names are stored as UTF-8. Lookups
  ignore case, as RISC OS does.
* **File types.** A file's type is its `,xxx` suffix: `!RunImage,ff8` is
  an Absolute file. A file with no suffix is typed from its extension,
  by a table, so `photo.jpg` is a JPEG. An extension never makes a file
  executable.
* **Dates.** Taken from the Linux modification time, kept to the second
  over a share to a Mac.
* **Attributes.** The RISC OS attribute byte — locked, readable,
  writable — is kept in an extended attribute on the Linux file, and
  read from the file's permissions where there is none.
* **Safety.** Nothing outside a disc's directory can be reached, even
  through a symbolic link. Locked files and open files are protected as
  RISC OS protects them.

### How it is tested

The same argument that kept FileCore out applies to HostFS: this is the
code that stands between a program and somebody's files, so it is held
to a conformance suite rather than to judgement.

That suite is about **690 tests**, each a script of filing-system
primitives with every answer known in advance, and it is not BOX's own
invention — it is the suite written for the team's HostFS for emulators.
Every test is run on the farm against real RISC OS 5.30, where it is
also run against RAMFS as a FileCore control, and then against BOX. A
test that the team's HostFS passes and BOX's fails is recorded as a
conformance gap, not argued away.

---

## 4. ResourceFS

ResourceFS is RISC OS's filing system for files held in memory. Modules
register blocks of files with it and it presents them, read-only, as
`Resources:`. The applications in the ROM — `Resources:$.Apps.!Edit`,
`!Draw`, `!Paint`, `!SparkFS` and the rest — and the ROM's own messages,
templates and sprites all live there.

BOX's ResourceFS is native C and works as RISC OS's does:

* Modules register files with `ResourceFS_RegisterFiles` in the RISC OS
  block format, invited by `Service_ResourceFSStarting`.
* Directories are implied by the files' names, and a file registered
  later hides an older one of the same name.
* Every write is refused with RISC OS's own error: "The filing system
  Resources: is read only".
* A file's handle is the address of its data in memory, which lets
  MessageTrans read message files where they lie, without copying them.
* When files come or go, ResourceFS tells the Filer, and open
  `Resources:` windows update.

The ROM's own files are packed in by a build tool and registered at
start-up by the Messages module, as on RISC OS.

---

## 5. SparkFS: the compressed filing system

SparkFS is RISC OS's compressed filing system, and an *image filing
system*: a file of a type it knows, such as a Zip archive, opens as a
directory. The Filer shows the archive's contents, and anything that
works on files — opening them, saving into the archive, dragging in and
out, `*Copy` — works inside it.

BOX carries RISC OS's own SparkFS, built from its C sources as modules
for the box, with all ten of its codecs:

| Archives | In BOX |
| --- | --- |
| Zip, Tar, Spark, PK ARC | read and written |
| ArcFS, LHA/LZH, ARJ, Zoo, StuffIt, PackIt, Compact Pro, Microsoft Cabinet, cpio, PackDir | read |

This matches what RISC OS 5.30's SparkFS supports, and archives were
checked against 5.30: the same listings, the same file types, and, for
archives BOX writes, the same bytes.

It is worth noticing why this one was comfortable to carry when FileCore
was not. SparkFS has published C sources, so it is compiled rather than
converted; and an archive is a file, written through the filing system
underneath, not blocks placed on a disc by hand.

For it, FileSwitch implements image filing systems as RISC OS does: a
path leading into an archive, such as
`HostFS::Host.$.Downloads.Fonts/zip.ReadMe`, opens the archive and hands
the rest of the path to SparkFS.

---

## 6. Booting and running from a USB disc

The commonest use of all this is to run RISC OS from a USB stick on a
PC. The stick holds two partitions:

* a small EFI partition holding the Linux kernel with the RISC OS ROM
  built into it, which the PC's firmware boots directly, with no boot
  loader;
* an ext4 partition named "RISC OS disc", which becomes
  `HostFS::Disc.$`.

At start-up BOX mounts the disc, runs its `!Boot` in the ordinary RISC
OS way — setting up `Choices$Write`, `Wimp$Scrap` and the rest — and
starts the desktop. From then on that disc is the machine's hard disc:
applications are installed by copying them to it, choices are saved on
it, and work is kept there.

There is a quiet benefit to the whole arrangement. Because the disc is
ext4 and not FileCore, it can be read on any Linux machine, and on a Mac
the RISC OS "disc" is simply a folder that the Mac's own tools can open.
If BOX ever lets you down, your files are not locked inside a format
that only BOX can read. That seemed the right property for an
experimental system to have.
