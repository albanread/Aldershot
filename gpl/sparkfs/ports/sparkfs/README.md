# !SparkFS: archives as directories

In the box's ROM: SparkFS and its ten codecs are ROM modules, and
!SparkFS is `Resources:$.Apps.!SparkFS`, in the Apps icon's directory
(#107; [In the ROM](#in-the-rom), below).

RISC OS's SparkFS (David Pilling's; RISC OS Open's
`FileSys/ImageFS/SparkFS` sources, in `third_party/SparkFS`) built for
the box: the **SparkFS** module (the filing system and its image filing
system), **SparkLib**, and all ten of its **codecs**, as C modules, and
**SparkFSApp**, its desktop front end (`!RunImage`: the icon bar icon, New
archive, encoding and decoding, Choices), as a C application -- for
either box, x32 on the Intel box, A64X32 on the Apple Silicon box. An
archive is then an image file: the Filer opens it as a directory, and
everything that works on files works inside it.

| Codec (module) | Archives | File type | Here |
| --- | --- | --- | --- |
| SparkZip (`Zip`) | Zip | &A91 Zip | read and written |
| SparkTar (`Tar`) | Tar | &C46 Tar | read and written |
| SparkSpark (`Spark`) | Spark, PK arc (ARC); ArcFS | &DDC Archive | Spark and PK arc read and written; ArcFS read |
| SparkLzh (`Lzh`) | LHA/LZH: `-lh0-` ... `-lh5-`, `-lzs-`, `-lz5-` | &DDC | read |
| SparkARJ (`ARJ`) | ARJ: methods 0-4 | &DDC | read |
| SparkZoo (`Zoo`) | Zoo: stored, LZW, lh5 | &DDC | read |
| SparkMcStuffit (`McStuffit`) | StuffIt (classic), PackIt, Compact Pro | &DDC | read |
| SparkCab (`Cab`) | Microsoft Cabinet: stored, MSZIP | &ABF Cabinet | read |
| SparkCPIO (`CPIO`) | cpio: odc, newc, crc, binary (both byte orders) | &ABA CPIO | read |
| SparkPackdDir (`PackdDir`) | PackDir | &68E PackdDir | read |

"Read" is what the codecs' `s/info` tables say (ReadOnly), as on RISC
OS 5.30. Phases 1 to 3 of the port are done (the modules, the codecs,
the application), and it is in the ROM; what is left (the filing-system
commands) is [#107](https://github.com/albanread/A7232ToolChain/issues/107).
MimeMap, which types a file by its extension where SparkFS's own list
does not, is the ROM's (#106, `modules/mimemap`).

## Building

    make [ARCH=aarch64]           the box, its ROM with SparkFS in it (sparkfs.mk)
    make sparkfs                  the Intel box's alone: x32
    make ARCH=aarch64 sparkfs     the Apple Silicon box's alone: A64X32

`sparkfs.mk` runs `build.sh` from `rosgd/` -- again whenever the port,
third_party's sources, the staged C or the runtime objects change; it
takes seconds -- into `build/ports/sparkfs` or
`build/aarch64/ports/sparkfs`: `modules/` (SparkFS and the ten codecs,
each `,ffa`), `obj/SparkFSApp/!RunImage,ff8` and `rom/Apps/!SparkFS`.
It needs the staged C headers (`make capps` for the machine, which make
does), the C runtime objects `roscc link --clib` takes
(`build/gen/capps/rt` or `build/a64x32/capps/rt`), roscc, and
`third_party/SparkFS` (THIRD_PARTY, else beside the toolchain:
`tests/lib/workspace.py third_party()`).

What build.sh does:

- **Stages** the sources as the DDE sees them, under `src/`: each
  component's `c/x` as `x.c`, `h/x` as `x.h`, its cmhg description;
  SparkFS's `h/SparkFS` as `Interface/SparkFS.h` and SparkLib's `h/` as
  `SparkLib/`, as their Makefiles export them, then applies `patches/`
  (below). All of the C compiles unchanged; three patches mend what ARM's
  code and Norcroft's layout happened to survive, and one sets SparkFS's
  memory for the ROM.
- **Compiles** with `capps.mk`'s flag table for the ABI (`make -f capps.mk
  capps-cflags`), with SparkFS's own warnings off (1990s C: implicit int,
  K&R casts), against the staged C: path and this port's `inc/`:
  `Global/RISCOS.h` (the one value SparkFS uses, UpCallV) and
  `Interface/MimeMap.h` (hdr/MimeMap's constants and its SWI), which the
  staged headers do not have.
- **cmhg** through roscc's `cmhg.py`, without international help (the
  Makefile's CMDHELP None: `-DNO_INTERNATIONAL_HELP`, the help text in the
  module).
- **Links** each codec as its Makefile builds it (CModule: its OBJS and
  `s/info`, `${ASMUTILS} ${SPARKLIB}`) with `roscc link --module --clib`,
  SparkLib as an archive (so SparkTar's and SparkCPIO's own `zfile` is
  taken before the library's), with
  ROSCC_X32_RT / ROSCC_A64X32_RT naming the box's runtime objects --
  roscc's default directory may hold older ones, whose module stub
  registers with SharedCLibrary but never initialises the library (no
  ctype tables: `atoi("1")` is 0; malloc waits for ever). A64X32's
  images go through `roscc a64x32-lint`.
- **Builds SparkFSApp** as its Makefile does (CApp, `CFLAGS -c90`,
  `CINCLUDES ${RINC}`): its 26 C files against RISC_OSLib's staged headers
  (`$CAPPS/inc/RISCOSLIB`) -- but linked with the C library alone, as its
  Makefile gives no `LIBS` (CApp's `APP_LIBS` is `${CLIB}`): it carries its
  own RISC_OSLib layer (`wz.c`, `wos.c`, `flex.c`) and the veneers it needs,
  its two ObjAsm files, here `appswi.c` and `apppoll.c` (below).
  `roscc link --clib`, an image (&FF8) on the ROM's SharedCLibrary;
  A64X32's through `roscc a64x32-lint`.
- **Makes `!SparkFS`** (below).

### The ObjAsm, in C

Each component has one ObjAsm file; here each is C, kept to what the
assembler does:

| File | The component's | What it is |
| --- | --- | --- |
| `runlink.c` | SparkFS `s/runlink` | SparkFS's call of a codec's link function: R12 the codec's workspace, a1 the block. In the box both modules are C of one ABI, so it is a C call; the file says why the codec's statics need no switching. |
| `sinterface.c` | SparkLib `s/sinterface` | the codec side: nineteen veneers (`veneer_loadfile` ...), each the ObjAsm's switch to the codec's statics then a branch through `fsentry_branchtable`; here each is the call |
| `zipinfo.c`, `tarinfo.c`, `sparkinfo.c`, `lzhinfo.c`, `arjinfo.c`, `cabinfo.c`, `cpioinfo.c`, `zooinfo.c`, `mcstuffitinfo.c`, `packddirinfo.c` | each codec's `s/info` | the tables a codec gives SparkFS (archive types and file types, compression methods, codes; Spark's conversion pairs), word for word: each DCD a word as written (its symbols kept), each DCB string packed into words little-endian as the assembler lays it down, ALIGN's padding; writable, as `|C$$data|` is (`*SparkFSMapFiletype` writes a file type into archivetypes) |
| `rminfo.c` | AsmUtils `s/rminfo` | `Image_RO_Base`, the module's base (cmhg.py's header is at offset 0) |
| `appswi.c` | SparkFSApp `s/swi` | the ten SWI veneers SparkFSApp brings (Acorn's "cwimp" s.swi cut down): `bbc_get`, `bbc_vdu`, `bbc_vduw`, `os_swi1`, `os_swix1`-`4` (one entry: R12 the SWI, R0-R5 from the arguments, XOS_CallASWIR12, MOVVC), `os_swi3r`, `os_swix3r` (three in, three pointers out, nothing stored on V) -- through the gate, as rosgd's `rlib/swi.c` makes RISC_OSLib's |
| `apppoll.c` | SparkFSApp `s/poll` | `wimp_poll` (the reason word stored before MOVVC: an error comes back), `wimp_pollidle` (its CMP clears V: always 0), the two floating point switches -- RISC_OSLib's 1992 s/poll, as `rlib/poll.c` |

### Patches

Applied to the staged sources, never to `third_party`:

| Patch | What |
| --- | --- |
| `t_accum-shift.patch` | SparkTar's and SparkCPIO's `t_accum` (Unix time to RISC OS's 5-byte time) splits a value into bytes with `n >> (i * 8)` for i up to 4: a shift by 32, undefined in C. ARM's shifter gives 0, which Norcroft's code relies on; x86 and AArch64 take the count modulo 32, and clang at -O2 dropped the fifth byte -- every date in a tar or cpio archive came out in 1900 (phase 1's Tar too). The shift is now taken only under 32. |
| `app-savestr-names.patch` | SparkFSApp's `saveblock[]` gives each save box entry's name as a string literal, ten of them `"            "`, and `setuparctypes()` `strcpy`s each archive type's name into them. Norcroft's code keeps those literals apart and RISC OS lets them be written; clang merges identical literals into one, so every entry was the last type's name: the New archive save box offered "Zip" five times where 5.30 offers Spark file, Spark dir, PK arc, Tar, Zip. `names` is now a 13-byte array per entry, from the same literal. |
| `rom-memory.patch` | SparkFS's `meminit` reads `SparkFS$Memory` as the module starts: SparkFSApp's `!Run` sets it just before it loads the module, -&2000000 on RISC OS 5 (a dynamic area of up to 32 MB); unset, it was a fixed 64K of the RMA. In the ROM SparkFS starts before any `!Run`, whose `RMEnsure` then finds it there, so unset is now what `!Run` would have set. A set value is used as before (`*RMReInit SparkFS` reads it again). |
| `cpio-hex-field.patch` | SparkCPIO's `loadheader` converts newc's and crc's 8-hex-digit fields with `strtoul` without ending them (the octal cases do), so whatever follows on the stack counts when it happens to be hex digits. RISC OS 5.30 itself calls a newc archive "Bad archive" once something has left digits there -- reading an odc archive does -- until a reset. The digits are now ended. |

## !SparkFS

`Resources:$.Apps.!SparkFS`, in the ROM (below): the application as
SparkFSApp's Makefile installs it (`INSTAPP_FILES` and the four
translations, each Messages with its `_Version` from VersionNum, as
Build:AwkVers makes it), laid out as RISC OS 5.30's `Utilities.!SparkFS`,
but for the modules, which are the ROM's:

- `!Boot` -- SparkFSApp's: the archive types' names, their run actions
  (`Run <Obey$Dir>.!Run %*0`: a double-click on an archive runs `!Run`
  with it), `SparkFS$Dir`, the icons (Themes). The Desktop runs it as it
  starts: it `*Filer_Boot`s every application in `Resources:$.Apps`, as
  RISC OS's Desktop does -- so archives are typed and open with a
  double-click from the desktop's start, with no `!Boot` line of the
  disc's (which has none: it would run twice).
- `!Run` -- SparkFSApp's, with the lines that would run ARM code or load a
  module changed, each with a `| ROSGD:` comment saying why:
  `Resources.IfThere` (an ARM utility, in 5.30's !SparkFS, not in the
  sources) is BootCmds' `*IfThere`; no ImageFSFix (an ARM module with no
  sources); `RMEnsure SparkFS 1.36 RMReInit SparkFS` for its `Run
  <SparkFS$Dir>.Resources.SparkFS`, as a ROM application's `!Run` starts
  its module (`RMEnsure !Edit 0.00 RMReInit !Edit`): nothing, unless
  Quit > FS too has killed it; and `WimpSlot` 512K, not 128K: the A64X32
  or x32 `!RunImage` is 141K and the C library's root stack 256K -- 448K
  is the least it starts in. Its FPEmulator lines and its MimeMap lines
  are SparkFSApp's: the ROM's FPEmulator (a stub, #114: the box needs no
  emulation) and MimeMap (#106) meet them, and `Inet$MimeMappings` is
  set already. The rest is as it is: `Resources.ResFind` (BASIC) sets
  `SparkFSRes$Dir`, `SparkFSImage 1`, the settings, `SparkFSAtExit`, then
  `!RunImage`.
- `!RunImage` -- SparkFSApp, above. It reads `Config.Choices` (or
  `Choices:SparkFS.Choices`): the memory SparkFS may use, the codecs to
  load (`WimpTask <SparkFS$Dir>.Modules.<codec>`, each), the archive type
  and method; then `Config.Extensions` and `Config.NoCo`
  (`*SparkFSExtension`, `*SparkFSNoCo`), and puts its icon on the icon bar.
  `Config.Choices` is SparkFSApp's with all ten codecs installed (its own
  installs Spark, Tar and Zip: the box's !SparkFS reads every archive type
  without a visit to Choices; Choices... still turns each off and on).
- `Modules` -- one Obey file a codec, named as the codec is (SparkFSApp
  lists the directory for Choices), which `*RMReInit`s the ROM's: started
  again, as loading a codec from here starts it afresh on RISC OS. A codec
  declares its archive types as image filing systems as it starts, once
  `SparkFSImage 1` has said to -- so they are started again after `!Run`.
  Quit > FS too `*RMKill`s them, as on RISC OS: the ROM's are then
  Dormant until `!SparkFS` runs again.
- `Load` -- this port's: SparkFS set up at the command line, without the
  desktop, as `!Run` and `!RunImage` set it up (`test/codectest.py`):
  SparkFS and the codecs started again, and what it obeys,
  `Resources.Extensions` and `Resources.NoCo`.
- `!Help` (this port's), `Licences` (each component's LICENCE).

## In the ROM

`sparkfs.mk` (#107), as the ROM's other C module, the ColourPicker, and
its other applications, !Edit, !Draw and !Paint, are put there:

- **The modules**: each `modules/<name>,ffa` a ROM unit
  (`tools/mkromx32.py`, `build/[aarch64/]gen/rom_spark*.c`) in a 256K slot
  of its own from &FCE00000, the Makefile's `SPARKFS_ROM`; started after
  the ColourPicker (`boot/rom_contents.c`) -- the runtime copies each to the
  RMA, as it does an x32 module the ROM holds (`runtime/rom.c`), and
  `*ROMModules` lists them. SparkFS first, then the codecs in
  `Config.Choices`'s order (Spark, Tar, Zip, Lzh, ARJ, Cab, CPIO, Zoo,
  McStuffit, PackdDir): SparkFS keeps each codec in the first free slot of
  its table, which orders New archive's types -- Spark file, Spark dir, PK
  arc, Tar, Zip, as on 5.30, where `!RunImage` loads them in that order --
  and a codec started again goes back to its slot. The image filing
  system is off until `!Run` turns it on (`SparkFSImage 1`), as when
  `!Run` loads the module; its memory as `!Run` sets it
  (`rom-memory.patch`).
- **!SparkFS**: `rom/Apps/!SparkFS`, one of `tools/mkresources.py`'s roots
  (the Makefile's `resources.c` rule), every file dated `SPARKFS_EPOCH`
  (the port's last commit, or SOURCE_DATE_EPOCH: the same ROM from one
  build to the next, #55).
- **The disc and the share**: no `!SparkFS`. `make usershare` takes away
  the copy in Apps that earlier shares were given; the disc's `!Boot` runs
  nothing of SparkFS's (the Desktop boots the ROM's).

The ROM grows by 1.7 MB: the modules' images 796K (A64X32; x32 760K), and
!SparkFS 969K -- its `!RunImage` 141K, the rest mostly Themes' sprites.

## What works (2 October 2026)

### Zip and Tar (phase 1)

FileSwitch's side is `modules/fileswitch/modfs.c` (its README: filing
systems from modules, image filing systems).

- **At the command line** (a box run with `rosgd.run`): `*FileInfo` of a
  zip made on the Mac is `DWR/ Directory`, OS_File 5 object type 3 and
  OS_File 20 &1000; `*Cat` and `*Ex` into it and its directories; `*Type`
  of a file in it; `*Copy` out of it (20000 random bytes, inflated, the
  same as the Mac's unzip gives); `*Copy` into it, a module of 109 KB
  deflated and an Obey file, and `*CDir` in it -- the Mac's unzip lists
  them and gives back the same bytes; `*SparkFSCreate 4` makes a new zip
  and `7` a new tar, filled the same way, read back on the Mac (unzip,
  tar). A tar made by macOS's tar lists, with its AppleDouble (`._`) and
  PaxHeader entries as SparkTar sees them.
- **In the desktop**, `tests/deskprobe/sparkfs.py`, 8 of 8 on both boxes
  (the Intel box under QEMU's TCG): a double-click on the zip, SparkFS not
  loaded, loads it and opens the archive's viewer; a double-click on
  `notes/txt` in it opens it in Edit; a line typed and saved with F3 is in
  the share's zip seconds later (SparkFS writes the archive when it closes
  the image); `Hello` dragged in from the Host viewer, and `Docs.Data,ffd`
  dragged out, byte for byte.
- **Against RISC OS 5.30** (the farm, with the dev system's
  Utilities.!SparkFS, the same set-up): the same zip gives the same
  `*FileInfo` and `*Ex` (types, attributes, dates, lengths), the same
  OS_File 5 and 20 results for the archive, a file and a directory in it
  (load, exec, length, attributes; type 3 and &1000 for the archive, 2 and
  &1000 for Docs, whose length is SparkFS's 20084), the same OS_GBPB 10
  object types (the archive 3), the same viewer in the Filer; a file
  copied in gets the same SparkFS extra field (`AC`, `ARC0`: load, exec,
  attributes) and a UTC time, as 5.30 writes it. The one difference was
  FileSwitch's, not SparkFS's: OS_File 20 of nothing (#105).

Notes:
- A name with `,xxx` in a zip (the HostFS convention, from a Mac) is a
  name: `ReadMe,fff` is a Data file called `ReadMe,fff`, on 5.30 too.
  Copied out to HostFS, its comma becomes U+F02C (HostFS's mapping of a
  character a host name cannot carry).
- Types come from the archive (SparkFS's `ARC0` field), else the
  extension through SparkFS's list (Config.Extensions), else MimeMap --
  the ROM's (#106): a `.png` is PNG, &B60 (`tests/deskprobe/sparkfs.py`'s
  pngtype) -- else Data.
- The archive's file is held open by FileSwitch while the image is open;
  SparkFS closes it after `SparkFSImageTimeout` (5 s) unused, and
  FileSwitch before anything opens, deletes or renames it.

### The codecs (phase 2)

`test/codectest.py` (in `make boxtest`): one or more archives per format,
opened, listed and copied out in the box -- by `tests/lib/boxrun.py`,
eight archives a boot, each boot a fresh SparkFS -- then the write round,
and everything compared:

    ROSGD_ARCH=aarch64 python3 ports/sparkfs/test/codectest.py   the Apple Silicon box
    python3 ports/sparkfs/test/codectest.py [--run-script S]      the Intel box
    python3 ports/sparkfs/test/codectest.py --refs-only           the archives against
                                                                  independent extractors
    python3 ports/sparkfs/test/codectest.py --farm claude3 [--record]   RISC OS 5.30

The archives (the source tree: text, random bytes, low-entropy data,
runs, an empty file, two levels of directories, RISC OS types):

- **Made by real tools**: cpio odc, newc and binary by macOS's bsdtar;
  stored and MSZIP cabinets by gcab (`test/data`); Spark archives (Spark
  file: methods 0, 1, 6, 7, 11, 12; PK arc: 0, 1, 6, 12) written by RISC
  OS 5.30's SparkFS on the farm (`test/data/530`); third_party's
  `SparkFSApp/test/NestedSpark.arc` (Spark archives inside a Spark
  archive). No other real-world RISC OS archive is on this disc.
- **Made by `test/archivers.py`** for the formats nothing on a Mac writes:
  LHA (header levels 0, 1, 2; `-lh0-`, `-lh4-`, `-lh5-`), Zoo (stored,
  LZW, lh5), ARJ (stored, method 1, method 4), StuffIt (stored, RLE90,
  LZW; folders, resource forks), PackIt (PMag, PMa4 Huffman), Compact Pro
  (RLE), ArcFS (stored, packed, compressed), PackDir (LZW) -- each with
  its real compression method, from the formats' published descriptions.
  `--refs` checks each with an extractor that is not SparkFS: lhasa and
  7zz (LHA), unar (Zoo, ARJ, StuffIt, PackIt, Compact Pro, Spark), 7zz
  (ARJ, cpio), cabextract (CAB): all of them give the files. ArcFS and
  PackDir have no extractor here (unar 1.10.8 has no ArcFS); their LZW
  is the one gzip's decompressor reads (compress), their RLE the one unar
  reads in StuffIt.

The checks: each archive a directory (`*FileInfo`), every file copied out
byte for byte the archive's, RISC OS types where the archive has them
(Spark, ArcFS, PackDir), the `*Ex` listings and every copied file's SHA-1
the same as RISC OS 5.30's (`test/data/farm-530.json`, `--farm --record`'s
run of the same script with the dev system's !SparkFS); and the write
round -- `*SparkFSCreate 1` (Spark) and `3` (PK arc) with each method
`*SparkFSMethod` writes, the tree copied in, read back in the box and
extracted by unar on the Mac. The Spark archives the box writes, read on
5.30 (`--farm --only box- --extra <box's SFSTEST.W>`), give the tree too.

Results, 2 October 2026: 53 of 53 on the Apple Silicon box and on the
Intel box (QEMU TCG); `tests/deskprobe/sparkfs.py` 10 of 10 on both
(its new steps: an LHA archive double-clicked in the Host viewer opens
as a directory; `Poem/txt` in it, `-lh5-` packed, opens in Edit).

What SparkFS does, on 5.30 as in the box (the test expects it):

- **StuffIt, Compact Pro: a folder in a folder.** SparkFS's `sitlist`
  counts the files in a folder but not the folders, so a nested folder is
  listed at the top, empty, and what follows it is misplaced;
  `comlist` reads Compact Pro's folder count (everything in the folder, as
  unar and macutils read it) as the folder's own objects. One level of
  folders is right. (`sit-nested`, `cpt-nested`: compared with 5.30.)
- **McStuffit's both-forks mode** (`*McStuffitMode 3`, its default:
  MacBinary, the resource fork first): a fork stored and read into memory
  is not padded, so the padding holds SparkFS's buffer's old bytes
  (compared without it); PackIt files get the length 128 + forks while
  their forks are laid out padded, so the data fork is cut short; and an
  empty PackIt file's Huffman-coded CRC takes under two bytes, its size
  comes out negative and copying it is "Not enough free memory" (5.30's
  *Copy then leaves the previous file's first 128 bytes in it; the box's
  leaves it empty).
- **SparkFS's memory**: with some twenty archives open at once, "Not
  enough free memory" -- hence a fresh SparkFS every eight archives.
- **A stored CAB**: SparkCab seeks to the folder's data plus the file's
  offset without stepping over the data blocks' headers, so the first
  file starts with an 8-byte block header (`cab-store`: 5.30's bytes).
- **CPIO**: each file in a subdirectory is listed twice.
- **PK arc** has no directories (`*CDir` in it: "Archive does not support
  this command") and keeps no RISC OS types.
- **Mac types**: McStuffit gives a file the type of its Mac file type
  through SparkFS's extension list (`TEXT` is Text).

Box faults found on the way: the dates (`t_accum-shift.patch`, above);
the native Wimp's *IconSprites before its RAM sprite area existed (a
!Boot at the desktop's start, before the first task) merged a file
called "WSpr" -- the archives' icons were "?" and !PDF's apptest failed
(#109, fixed: `modules/wimp/sprites.c`, a self-test check); FileSwitch's
`*Copy` totals and its missing per-directory line (#110, #111, open).

### The application (phase 3)

SparkFSApp in the desktop, `tests/deskprobe/sparkfs.py`'s app phase (21
of 21 with the files phase, on both boxes; the Intel box under QEMU's
TCG): !SparkFS double-clicked in Apps puts its icon on the icon bar; its
menu and Info window; New archive's save box makes an empty Zip, Tar,
Spark and PK arc archive -- each byte for byte what RISC OS 5.30's
SparkFS makes -- and opens it; Hello and a directory tree dragged into
the new zip's viewer (the zip on the share holds them, byte for byte;
`unzip -t` and `7zz t` pass it); Hello dropped on the icon and saved as
Compress, GZip, UUcode, BtoA, Mime and Boo, each 5.30's bytes (GZip's and
MIME's date stamp apart); a BtoA and a uuencoded file dropped on the icon
decoded where they are, as 5.30 decodes them; the tree dragged out of the
zip; Choices; Quit > FS too (SparkFS and its codecs killed); !SparkFS run
again (its codecs loaded afresh, the old ones killed) and a zip opened.
The first phase's double-click on a zip now runs the application too.

**Against RISC OS 5.30** (the farm, the dev system's `Utilities.!SparkFS`,
1.56; the box's is 1.59 from the sources): the icon bar menu, Info, New
archive's save box (Spark file, Spark dir, PK arc, Tar, Zip), the file
save box (Compress ... DeBoo) and the Choices window are the same (5.30's
Choices ticks only Spark, Tar and Zip: its default Choices); the empty
archives and the encodings are the same bytes (the references in the
probe); what 5.30's SparkFSApp does that may surprise, the box does too:

- decoding is in place: Hello/ab becomes its bytes and a zero byte (btoa's
  last group), a Data file; a uuencoded file gives the name in its begin
  line, less `/uue`, a Data file beside it (`Hello`, overwriting Hello);
- a MIME file dropped on the icon is not recognised (SparkFSApp's MIME
  test is commented out): its save box offers to encode it;
- after Quit > FS too, a double-click on an archive the Filer had shown as
  a directory is "Error when reading ... is a file" (the Filer's cache);
- a drop on a directory's icon copies into the viewer's directory, not
  into that directory (the Filer's).

Box faults found on the way, fixed:

- **A BASIC program run from an Obey file in a Wimp task never came back**
  to the file: !SparkFS's `!Run` stopped after `Run ...ResFind`. RISC OS's
  Obey module puts its own exit handler in front of the application's
  starter's when an application starts on a line with more lines after it
  (s.Obey: MyExitHandler goes on with the next line); the box's Obey now
  does the same (`runtime/filecmds.c`, `ros_obey_catch_exit`, set by
  `ros_module_run_as_application`). 5.30, the box at the command line and
  now the box in a Wimp task all go on.
- **Killing a C module was "undefined instruction"**: `_clib_finalisemodule`,
  the library slot every cmhg finalisation veneer calls, was not in the
  ROM library yet (package L5). It is now (`clib/cl_body.c`: the atexit
  functions, the allocator's end, 0), with a self-test check (modmalloc).
  SparkFSApp kills and reloads its codecs when it runs again and on Quit >
  FS too.
- `tests/deskprobe/sparkfs.py`'s share `!Boot` sets `Choices$Path` and
  `Wimp$Scrap`, as the disc's does: SparkFSApp's `!Run` reads
  `Choices:SparkFS.AtExit` (`*IfThere` of a path that is not set is an
  error, on 5.30 too).

Not here yet (#107): `*Mount`/`*Dismount` (filing-system commands, which
the box's command line does not look up).

### In the ROM (#107)

`tests/deskprobe/sparkfs.py`, 24 of 24 on the Apple Silicon box and on
the Intel box (QEMU's TCG), its shares with no `!SparkFS`: at the
desktop's start SparkFS and its codecs active ROM modules and the ROM
!SparkFS's `!Boot` run (rom); the archives typed and opened by a
double-click; a zip's `Picture.png`, untyped in the archive, typed PNG by
MimeMap (pngtype); `!SparkFS` in the Apps icon's directory, a
double-click there putting it on the icon bar (appstart), the ROM's copy
running (approm); Quit > FS too leaving the ROM's modules Dormant, and
`!SparkFS` run again from Apps starting them (rerun).
`test/codectest.py`, through the ROM's `Load`: 53 of 53.

## Licences

SparkFS, SparkLib and the codecs: CDDL 1.0 and Apache 2.0, file by file,
with the Info-ZIP licence for SparkZip's compression code -- each
component's `LICENCE`, which `!SparkFS.Licences` carries. The files here
translating their ObjAsm keep the original's notice and licence; the rest
of this directory is the toolchain's.
