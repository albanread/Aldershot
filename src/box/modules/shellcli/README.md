# ShellCLI

A native C version of the RISC OS 5 ShellCLI module (`Desktop/ShellCLI`, 0.39). It gives the desktop a command line. The Task Manager's F12 key, and *Commands on its Tasks window menu, start the task `ShellCLI`. The shell writes VDU 4 text over the bottom of the desktop screen and reads commands until Return is pressed on an empty line. The Wimp then redraws the screen. Nothing else runs while the shell is active, as on RISC OS.

## Command and SWIs

| Item | Meaning |
| --- | --- |
| `*ShellCLI` | Enters the module as the application and runs the shell. With a shell already active it gives error &900. |
| `Shell_Create` (&405C0) | Sets the environment handlers to their defaults, except the exit and error handlers, which become the shell's. |
| `Shell_Destroy` (&405C1) | Puts the saved exit handler back and frees the shell's block. |

The SWIs are defined in `api/defs/shellcli.toml`.

The prompt is `CLI$Prompt` (default `*`). The greeting is `CLI$Greeting`, set at start from the Messages file (`Resources:$.Resources.ShellCLI.Messages`). A program that ends with OS_Exit returns to the shell's command loop.

Service calls: Service_Memory (&11) is claimed while the shell starts up. Service_WimpCloseDown (&53) is refused with &104 "Window Manager is in use" when a Wimp program is started from the shell's prompt.

Errors: &900 ShellCLI not active, &901 ShellCLI task is still active (on finalising with a shell), and &104 Window Manager is in use.

## Differences from RISC OS 5.30

- The veneer that resets the private stack is replaced. The loop keeps its place on the task's thread (a `jmp_buf`), and the exit and error handlers go back to it. TaskWindow does the same.
- The help string ends in "ROSGD native".

Tests: `tests/desktop/shellcli/probe.py` reads the screen after F12 and compares it with `expected/farm.txt`, taken from RISC OS 5.30. `boot/selftest_shellcli.c` checks the rest.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
