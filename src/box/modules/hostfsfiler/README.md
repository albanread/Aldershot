# HostFSFiler

HostFSFiler puts HostFS's discs on the icon bar and opens their roots in
the Filer. It is a native module in C (`hostfsfiler.c`) that follows the
emulator's HostFSFiler 2.00, and RAMFSFiler (`FileSys/RAMFS/RAMFSFiler`)
for the pattern of the services and the private word. The Filer
(`Desktop/Filer`) starts it with Service_StartFiler.

## Use

- Command: `*Desktop_HostFSFiler`. It takes no parameters. Called with no
  workspace it gives "Use *Desktop to start HostFSFiler".
- No SWIs. Services: Reset, StartFiler, StartedFiler, FilerDying.
- The task is called `HostFS Filer`. It makes one icon bar icon for each
  HostFS disc, up to eight, with the disc's name as its text.
- Click SELECT or ADJUST on an icon to open the disc's root,
  `HostFS::<disc>.$`, by sending Message_FilerOpenDir to the Filer. MENU
  gives `Open`, `Free` and `Quit`. Free runs `*ShowFree -FS HostFS <disc>`.
- Files dropped on an icon are saved or copied to the root of that disc.
- Help requests are answered for the icons and the menu.
- Quit ends the task. HostFS stays, and the Filer can start the task
  again.

The discs are read when the task starts. A disc mounted later gets no
icon until the task starts again. They come from `ros_hostfs_disc()` in
`modules/fileswitch`.

## Differences from RISC OS 5.30

- There is more than one icon when there is more than one disc. With one
  disc the module behaves as the emulator's.
- There is no Dismount item. A HostFS disc is a Linux mount that the
  desktop cannot make again.
- The menu says `Open`, as the emulator's module does, not `Open '$'`.

## Tests

`tests/desktop/hostfsfiler` compares the module with RISC OS 5.30.
`boot/selftest_hostfsfiler.c` checks it in the runtime.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
