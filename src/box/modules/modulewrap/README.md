# ModuleWrap: !Edit, !Draw, !Paint and Filer_Action

The RISC OS ROM holds its C applications twice. Their files are in
ResourceFS (`Resources:$.Apps.!Edit` and `Resources:$.Resources.Edit`), and
their program is a module that RISC_OSLib's ModuleWrap (`s/modulewrap`)
makes of it. The ROM `!Run` ends by running that module's command, for
example `Desktop_Edit %*0`.

Here the program is an x32 or A64X32 image, carried in the ROM as a file
such as `Resources:$.Resources.Edit.!RunImage` (type &FF8,
`tools/mkromapps.py`). The native module in `modulewrap.c` is ModuleWrap's
shape around that image. The ROM `!Run` files are RISC OS's own, except
that Edit's `Run Edit:Export` line, which calls ARM code from BASIC, is
replaced by `SetEval Edit$Tokenise -1` and `SetEval Edit$Detokenise -1`.

## Use

| Module | Command | Help string version |
| --- | --- | --- |
| `!Edit` | `*Desktop_Edit [<args>]` | 1.76 (15 Jun 2026) |
| `!Draw` | `*Desktop_Draw [<args>]` | 1.46 (19 Aug 2023) |
| `!Paint` | `*Desktop_Paint [<args>]` | 2.59 (01 Nov 2025) |
| `Filer_Action` | `*Filer_Action` | 0.65 (28 Jun 2025) |

Each command takes 0 to 255 parameters, not GSTransed. It sets the
`WimpSlot` for the image (640K, set by `MODULEWRAP_*_SLOT_K`), then runs
the image with the arguments as the task's application. The command
returns when the image ends. There is no workspace until the application
runs. The modules start where the Pi ROM puts them (`boot/rom_contents.c`):
`!Edit`, `!Draw` and `!Paint` after DHCP, and Filer_Action after Percussion.

The help and syntax texts are in each application's
`Resources:$.Resources.<Name>.Messages`, as in 5.30. `*Help Desktop_Draw`
runs on to the end of the file, as 5.30's does.

Filer_Action is the second ModuleWrap shape. At start it sets
`FilerAct$Path` to `Resources:$.Resources.FilerAct.` unless it is set. The
Filer starts it with Wimp_StartTask for every copy, move, delete, count,
access, set type, stamp and find. Each operation is a task with its own
application space. `ports/filer_action/README.md` describes its files.

## Differences from RISC OS 5.30 and what is not done

- The application directory's files are not registered with ResourceFS by
  the module. The ROM has them in its one ResourceFS block, so
  `*RMKill !Edit` leaves them where they are.
- Service_Memory is not claimed, and the module does not refuse to die
  while its application runs. It has no numbered incarnations
  (`!Edit%W0`). A second copy is a second task.
- The slot is 640K, not 5.30's 40K, because the code is in the image and
  not in the ROM.
- The help string versions of Edit and Paint are those of the ROOL
  sources used. Paint's source is newer than 5.30's (2.54).
- With `Edit$Path` unset the image starts and fails in `msgs_init`, as in
  5.30. The error reaches the task's error handler as one Cancel box, not
  5.30's OK box followed by "Return code too large".

## Tests

- `boot/selftest_resfiler.c`: `test_rom_edit`, `test_rom_draw`,
  `test_rom_paint` and `test_rom_fileract` check the files, the `!Run`
  lines, the image, the module, its help and RMKill with RMReInit.
- `boot/selftest_oscli.c`: commands that never return leave no alias
  expansion and no RMA behind.
- `tests/desktop/romapps/check.py` (`edit`, `draw`, `paint`, `fileract`)
  compares the files and the modules with RISC OS 5.30's ROM.
- `tests/deskprobe/edit.py`, `paint.py` and `fileraction.py` start each
  application from the desktop and use it (`make boxtest`).

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
