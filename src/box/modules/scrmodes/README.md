# ScreenModes

A native replacement for the part of RISC OS ScreenModes that the BOX still needs. On RISC OS the module reads monitor description files and owns the mode list. In the BOX the runtime (`runtime/vdu/modes.c`) enumerates the display's own modes, so only the SWI chunk (&487C0) remains. The Display Manager uses it to ask for the monitor's name.

| SWI | Behaviour |
| --- | --- |
| `ScreenModes_ReadInfo` | R0 = 0: the display's own name (the DRM connector, for example "Virtual-1"). R0 = 1: DPMS state, always 0. R0 = 2: speaker mask, 0 and 0. |
| `ScreenModes_EnumerateAudioFormats` | Returns an empty list. |
| `ScreenModes_Features` | Returns 0. |

There are no commands. `*LoadModeFile` and `*SaveModeFile` are not provided, because the BOX has no monitor description files.

Without a display, ReadInfo gives "No monitor description file loaded" (&BF4). An unknown item gives "Unknown ScreenModes_ReadInfo call" (&BF6).

Source followed: ScrModes in the RISC OS Open sources (`Sources/Video/UserI/ScrModes`), version 1.65.

Tests: `boot/selftest_display.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
