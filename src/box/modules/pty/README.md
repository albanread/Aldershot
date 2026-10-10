# PTY

PTY runs a Linux program on a pseudo-terminal and gives RISC OS
non-blocking reads and writes of it. RISC OS has no module like it. The
nearest is TaskWindow. PTY exists because the SSH client is OpenSSH's
`ssh`, a Linux program that needs a terminal. `term.c` adds a VT100
terminal drawn with VDU codes, which `*SSH` and `*SSH-KeyGen` (InetRes)
use.

## Use

The SWIs are the box's own, at &C00C0, typed raw in `api/defs/pty.toml`.
Errors are &C00C0 plus a number.

| SWI | In | Out |
| --- | --- | --- |
| PTY_Open | R1 command line, R2 columns, R3 rows | R0 handle |
| PTY_Read | R0 handle, R1 buffer, R2 size | R3 bytes read. 0 means nothing yet. It never waits. |
| PTY_Write | R0 handle, R1 bytes, R2 count | R3 bytes taken |
| PTY_Status | R0 handle | R1 1 once ended and read dry, R2 exit status (128 + n for signal n), R3 bytes waiting |
| PTY_Resize | R0 handle, R2 columns, R3 rows | the program gets SIGWINCH |
| PTY_Close | R0 handle | R2 exit status. SIGHUP, then SIGKILL after a second. |

The first word of the command line names the program, found on
`/usr/bin:/bin`. The program is a child of `/init`, a session leader with
the pseudo-terminal as its controlling terminal. `TERM` is `vt100`. If
exec fails, PTY_Open reports the error, for example "ssh: No such file or
directory".

The terminal (`pty_terminal`) draws the program's output in the text window
from the cursor. It supports cursor movement, scrolling regions, erasing,
colours (SGR 30 to 37, 90 to 97, 256 colour and RGB through ColourTrans),
bold, reverse, and replies to DSR and DA. UTF-8 is drawn as RISC OS Latin-1.
A wide character is drawn as `??`. The alternate screen is a cleared screen.
Keys come from OS_Byte 129, and the cursor keys become escape sequences.
The session ends when the program ends. In a task window the terminal is a
plain stream, with escape sequences left out.

The box provides (`boot/main.c`) devpts at `/dev/pts`, an `/etc/passwd` that
names root with the share `/host` as home, so keys and `known_hosts` live in
`/host/.ssh`, and `/usr/bin/ssh` and `/usr/bin/ssh-keygen`, OpenSSH 10.5p1
built as static musl binaries by `deps/build-openssh.sh`.

## Differences from RISC OS 5.30 and what is not done

There is no PTY module in RISC OS. Not done:

- A desktop terminal, a Wimp task drawing the VT100 in a window.
- The text under the alternate screen is not kept. Underline, italic and
  blink are not drawn.

## Tests

`boot/selftest_pty.c`. `boot/selftest_sshd.c` covers the SSH server.

OpenSSH is under its own BSD style licences.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
