# BootCommands

A native module in C (`bootcmds.c`) that follows ROOL's `Programmer/BootCmds`
1.54. It was checked against RISC OS 5.30's version 1.53. It has no SWIs.
C code reaches the same work through `bootcmds.h`. The messages are in
`Resources:$.Resources.BootCmds.Messages`.

## Commands

| Command | What it does |
|---|---|
| `*AddApp <application>` | Adds `!Boot`, `!Help` and `!Run` Obey files to `Resources:$.Apps` |
| `*AppSize`, `*AppSlot`, `*FreePool`, `*ShrinkRMA`, `*AddToRMA` | Memory commands |
| `*Do <command>` | GSTranses the tail, then runs it |
| `*X <command>` | Runs a command and keeps its first error in `X$Error` |
| `*IfThere <file> then <command> [else <command>]` | Runs one command or the other |
| `*LoadCMOS <file>`, `*SaveCMOS <file>` | Restore or save the configuration |
| `*Repeat <command> <directory> [options]` | Runs a command for each object in a directory |
| `*SafeLogon ...` | Logs on to a file server |
| `*AppPath`, `*PrepPath`, `*RemPath`, `*Canonical` | Edit path variables |

There are seventeen commands, in 1.54's order. A wrong argument count gives
the syntax from the Messages file. The errors are `&81F300` (Corrupt CMOS
file), `&81F301` (CMOS file is for a different OS version), `&81F302` (Not
enough memory) and `&81F303` (System variable must contain a string). `*Do`
can return `&1E4 Buffer overflow` and `*Repeat` can return `&800E06 Return
code too large`.

## Differences from RISC OS 5.30

- `*AppPath`, `*PrepPath`, `*RemPath` and `*Canonical` are new in 1.54. 5.30
  does not have them.
- `*Repeat` runs inside the module. On RISC OS it is an application that loads
  at &8000 over the caller. It also lists any number of objects, where the
  application stopped at 32768. With bad arguments it writes
  `Repeat: <error>`, where the application could show a Wimp error box and
  hang a farm job.
- `*PrepPath` terminates its result. 1.54 does not.
- `*SafeLogon` always runs `*%Logon`. There is no NetFS in the box.
- The memory commands report RISC OS's errors but work on the box's memory.
  The RMA is a single 256 MB reservation, there is no free pool, and
  `*AppSize` does not move anything.

## Tests

`tests/desktop/bootcmds` compares the module with RISC OS 5.30.
`boot/selftest_bootcmds.c` checks what must always hold, including the four
commands that 1.53 lacks.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
