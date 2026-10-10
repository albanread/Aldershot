# DisplayManager

The monitor icon at the right of the icon bar, which chooses the screen mode.
It is a hand conversion to C of RISC OS 5.31's DisplayManager
(`Sources/Video/UserI/Display`, nine ObjAsm sources). Every routine is a
function in `display.c`, with the same shape and behaviour. The task that
`*Desktop_DisplayManager` starts is C too. It uses the ROOL `Messages` and
`Templates` files, which are in ResourceFS as
`Resources:$.Resources.Display.*`.

## Use

- The module has no SWIs. Its one command is `*Desktop_DisplayManager`, which
  the Desktop module starts at `Service_StartWimp`.
- Select on the icon opens the Display window. Menu opens the icon bar menu
  (Info, Mode). The window has colours, resolution and rate menus, and an OK and
  a Cancel button. The Mode dialogue takes a mode string and runs `*WimpMode`.
- The module handles Service_Reset, ModeChange, StartWimp, StartedWimp,
  CalibrationChanged, WimpPalette and ModeFileChanged.
- Errors are `ErrorBase_Modes` (&C00 up), looked up in the module's Messages.

The mode list comes from `OS_ScreenMode 2`. It holds the display's sizes at
every pixel format that the driver lists (GraphicsV 17). The mode strings come
from `OS_ScreenMode 13` and `14` (`runtime/vdu/modes.c`), which follow the
kernel's `ScreenMode_ModeStringToSpecifier` and `ScreenMode_ModeSpecifierToString`.

## Differences from RISC OS 5.30

- The mode list is the display's own, not ScreenModes' monitor file list. The
  rate menu holds the DRM connector's rates, mostly 60 Hz.
- `*LoadModeFile` does not exist. Dragging a monitor description file to the
  icon shows the command's error.
- The help string is RISC OS's version and date, marked `ROSGD native`.

## Tests

`boot/selftest_display.c`, hosted and in the box. It covers the mode
enumeration, the mode strings, `OS_ScreenMode` selection, the module's command,
path variable and resources, and the Service_StartWimp claim. The deskprobes
click the icon and menus and change mode, and compare with RISC OS 5.30.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
