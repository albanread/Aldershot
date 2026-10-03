# HostFS and the Filing Systems

*3 October 2026*

## Introduction

On a Raspberry Pi, RISC OS keeps its files on Acorn's own filing
systems: FileCore discs on an SD card or a USB drive, reached through
SDFS or SCSIFS. BOX has none of these. It keeps its files on Linux's
filing systems instead, and shows them to RISC OS through HostFS.

This article describes:

* FileSwitch, which BOX rewrote, and the filing systems under it
* HostFS, and how it maps Linux files to RISC OS files
* ResourceFS, the read-only filing system holding the ROM's files
* SparkFS, the compressed filing system, which opens archives as
  directories
* the commonest use of all of this: booting and running RISC OS from a
  USB disc.

---

## 1. No Acorn filing systems

BOX does not implement FileCore, or any of the filing systems built on
it: ADFS, SCSIFS, SDFS and RamFS. Nor does it have CDFS.

The reasons are practical. FileCore exists to manage a disc's blocks
itself, and its drivers talk to the hardware. In BOX, Linux already
does both, for many more kinds of disc than RISC OS ever supported.
Translating FileCore would have meant writing block drivers for Linux's
devices, only to put a second filing system on discs Linux could
already read and write.

The cost is that a FileCore disc or disc image cannot be read in BOX.
Files must be copied off it first, on a machine that can read it.

---

## 2. FileSwitch

FileSwitch is the module that owns RISC OS's file SWIs: `OS_File`,
`OS_Find`, `OS_Args`, `OS_GBPB`, `OS_BGet`, `OS_BPut` and
`OS_FSControl`. It parses names, handles path variables and wildcards,
keeps open files, and passes each operation to the filing system
concerned.

BOX's FileSwitch is a rewrite in C, made from FileSwitch's own sources
and the PRM rather than translated. It keeps RISC OS's rules: the same
names, the same errors and error numbers, the same stream behaviour,
and the same UpCalls to tell the Filer that a file has changed, so that
Filer windows update themselves.

Under it are three filing systems of its own (HostFS, ResourceFS, and
LanMan for SMB shares), and any filing system or image filing system a
module adds in the usual way, through `OS_FSControl`.

---

## 3. HostFS

HostFS is the filing system for files that Linux holds. Each Linux
mount it is given is one HostFS disc:

| Disc | What it is |
| --- | --- |
| `Host` | on a Mac, the host's own folder, shared into the box (9P or virtiofs) |
| `Disc` | the RISC OS disc: the ext4 partition of the USB stick on a PC |
| others | SMB shares from Windows, Macs or Linux, mounted by `*LMConnect` |

`HostFS::Host.$.Apps` is the `Apps` directory of the shared folder, and
`HostFS:$` is the first disc. Whatever filing system Linux can mount
(ext4, a Mac's folder, an SMB share) becomes a RISC OS disc in the same
way.

### Mapping names, types and attributes

HostFS follows the conventions already used by the team's HostFS for
emulators, so a folder reads the same in either:

* **Names.** A RISC OS `/` is a Linux `.`, so `ReadMe/txt` is
  `ReadMe.txt` on the host. Latin-1 names are stored as UTF-8. Lookups
  ignore case, as RISC OS does.
* **File types.** A file's type is its `,xxx` suffix: `!RunImage,ff8`
  is an Absolute file. A file with no suffix is typed by its extension,
  from a table, so `photo.jpg` is a JPEG. An extension never makes a
  file executable.
* **Dates.** Taken from the Linux modification time. Over a share to a
  Mac, dates are kept to the second.
* **Attributes.** The RISC OS attribute byte (locked, readable,
  writable) is kept in an extended attribute on the Linux file, and
  read from the file's permissions where there is none.
* **Safety.** Nothing outside a disc's directory can be reached, even
  through a symbolic link. Locked files and open files are protected as
  RISC OS protects them.

HostFS is held to a test suite of about 690 cases, written against
FileCore's behaviour on RISC OS 5.30.

---

## 4. ResourceFS

ResourceFS is RISC OS's filing system for files held in memory. Modules
register blocks of files with it, and it presents them, read-only, as
`Resources:`. The applications in the ROM (`Resources:$.Apps.!Edit`,
`!Draw`, `!Paint`, `!SparkFS` and the rest) and the ROM's own
messages, templates and sprites all live there.

BOX's ResourceFS is native C, and works as RISC OS's does:

* Modules register files with `ResourceFS_RegisterFiles`, using the
  RISC OS block format, and are invited to do so by
  `Service_ResourceFSStarting`.
* Directories are implied by the files' names. A file registered later
  hides an older one of the same name.
* Every write is refused with RISC OS's own error: "The filing system
  Resources: is read only".
* A file's handle is the address of its data in memory. MessageTrans
  uses that to read message files where they lie, without copying them.
* When files come or go, ResourceFS tells the Filer, which updates any
  open `Resources:` windows.

The ROM's own files are packed into the ROM by a build tool, and
registered at start-up by the Messages module, as on RISC OS.

---

## 5. SparkFS: the compressed filing system

SparkFS is RISC OS's compressed filing system. It is an *image filing
system*: a file of a type it knows, such as a Zip archive, opens as a
directory. The Filer shows the archive's contents, and anything that
works on files (opening them, saving into the archive, dragging files
in and out, `*Copy`) works inside it.

BOX carries RISC OS's own SparkFS, built from its C sources as modules
for the box, with all ten of its codecs:

| Archives | In BOX |
| --- | --- |
| Zip, Tar, Spark, PK ARC | read and written |
| ArcFS, LHA/LZH, ARJ, Zoo, StuffIt, PackIt, Compact Pro, Microsoft Cabinet, cpio, PackDir | read |

This is as RISC OS 5.30's SparkFS supports them, and archives were
checked against 5.30 with SparkFS: the same listings, the same file
types, and, for archives BOX writes, the same bytes. SparkFS and its front end, `!SparkFS`, are in the ROM.

For this, FileSwitch implements image filing systems as RISC OS does: a
path that leads into an archive, such as
`HostFS::Host.$.Downloads.Fonts/zip.ReadMe`, opens the archive and
hands the rest of the path to SparkFS.

---

## 6. Booting and running from a USB disc

The commonest use of all this is to run RISC OS from a USB stick on a
PC. The stick holds two partitions:

* a small EFI partition, holding the Linux kernel with the RISC OS ROM
  built into it. The PC's firmware boots it directly, with no boot
  loader.
* an ext4 partition named "RISC OS disc", which becomes
  `HostFS::Disc.$`.

At start-up BOX mounts the disc, runs its `!Boot` in the ordinary RISC
OS way (which sets up `Choices$Write`, `Wimp$Scrap` and the rest) and
starts the desktop. From then on, the disc is the machine's hard disc:
applications are installed by copying them to it, choices are saved on
it, and work is kept there. Because it is ext4, the stick can also be
read on any Linux machine.

On a Mac, the arrangement is the same, but the disc is a folder on the
Mac shared into the box as `Host`, so the Mac's own tools can work on
the same files that RISC OS sees.
