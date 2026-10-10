# FileSwitch and HostFS

FileSwitch owns the RISC OS file SWIs. It is a native module in C
(`fileswitch.c`). Beneath it are the native filing systems, behind the
interface in `fileswitch.h`:

- **HostFS** (`hostfs.c`): the host's files, one disc per mount.
- **ResourceFS** (`modules/resourcefs`): the ROM's files, read only.
- **LanMan** (`modules/lanmanfs`): HostFS's discs under LanManFS's name.

Filing systems and image filing systems that modules add come in through
`modfs.c`. The file utilities are in `fsutils.c`.

It follows the ROOL sources in `FileSys/FileSwitch` for the SWIs, the
stream rules, the error numbers and the messages. The host half of the
emulator's HostFS decides how names, types, dates and attributes map to
host files, so a share reads the same here as in the emulator.

## Use

FileSwitch claims FileV, ArgsV, BGetV, BPutV, GBPBV, FindV and FSCV. It
provides:

- `OS_File` (0 to 24, 255), `OS_Find`, `OS_BGet`, `OS_BPut`, `OS_Args`
  (0 to 9, &FE, &FF), `OS_GBPB` (1 to 12).
- `OS_FSControl` 0 to 10, 12 to 28, 31 to 33, 35 to 40, 43 to 45, 47, 49
  and 55.
- `*Access`, `*Back`, `*Cat`, `*CDir`, `*Copy`, `*Count`, `*Dir`, `*Ex`,
  `*FileInfo`, `*Info`, `*LCat`, `*LEx`, `*Lib`, `*NoDir`, `*NoLib`,
  `*NoURD`, `*Rename`, `*Run`, `*SetType`, `*Shut`, `*ShutDown`, `*Stamp`,
  `*Up`, `*URD`, `*Wipe`, `*HostFS` and `*ResourceFS`.

Names are `[-fs-|fs[#special]:|path:][:disc.][$|&|@|%|\][.a.b^.c]`.

HostFS has one disc per Linux mount. `ROSGD_DISCS` (`Name=/dir,...`)
names them. Without it the host share `/host` is `Host` and the disc
mounted at `/disc` is `Disc`.

On the host a RISC OS `/` is a `.`. Acorn Latin-1 is UTF-8. A file's type
is its `,xxx` suffix, or else its extension looked up in `typemap.txt`.
The attribute byte is kept in the extended attribute `user.riscos.attr`
(`riscos.attr` on macOS), or taken from the mode bits when there is none.
Nothing outside a disc's directory can be reached, including through
symlinks. Errors are filing system 220, `&0001DCxx`.

FileSwitch issues `OS_UpCall 3` (UpCall_ModifyingFile) before each change
to a filing system, so that the Filer redraws its viewers. ResourceFS does
the same when files are registered and deregistered.

HostFS registers with the Free module for `*ShowFree`.

## Filing systems from modules

`OS_FSControl` 12 and 16 add and remove a filing system. 35 and 36 add and
remove an image filing system. A module's entries are called as in
`s/LowLevel`, with buffered or unbuffered streams as the open asks.
A file whose type an image filing system claims is object type 3, and a
path that goes into it opens the image. SparkFS (`ports/sparkfs`) is the
real client.

## Differences from RISC OS 5.30 and what is not done

- There is no buffering on HostFS. Every byte is a call.
- Directories have load and exec addresses of 0, as the emulator's HostFS
  gives them.
- A directory argument that names nothing gives the filing system's "Not
  found". RISC OS may say "Directory not found". This has not been checked
  on RISC OS.
- Over 9p to a macOS host, dates are kept to the second.
- Special fields (`fs#special:`) are not passed to a module's filing
  system. Discs on a module's filing system are not parsed. A module's
  filing system commands are not looked up. `fsinfo_alwaysopen`,
  `nullnameok`, `handlesurdetc` and the MultiFS extensions are not used.
- Free space on a module's filing system comes from FSEntry_Func 30 only.
- `OS_FSControl 13` returns a block of FileSwitch's own in R2, not the
  filing system's control block.

## Tests

- `boot/selftest_files.c`: the file SWIs and commands.
- `boot/selftest_imagefs.c`: a test filing system and a test image filing
  system, both entered through `ros_call`.
- `tests/hostfs/rosgd_suite.py` runs the emulator's HostFS suite (about
  700 cases) against this module. Its guest program is `boot/hfstest.c`.
  Hosted, 694 cases pass and 3 are known limits of the suite. In the box,
  over 9p to macOS, 2 fail: a date before 1970 and the volume's exact size,
  which 9p cannot carry.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
