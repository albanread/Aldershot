# SSHD

An SSH server for the BOX. RISC OS has none. This is OpenSSH's `sshd`, a Linux daemon started by `/init`, with RISC OS behind it. A login gives a `*` prompt, which is a RISC OS command line.

```
*SSHD [start] [-p <port>]    start sshd, on port 22 by default
*SSHD stop
*SSHD status
```

From the Mac, `run/run-x86_64.sh` forwards 127.0.0.1:2222 to port 22 of the BOX, starts `*SSHD` at boot (`rosgd.sshd`) and adds the Mac user's public key to `.ssh/authorized_keys` on the share.

```
ssh -p 2222 root@127.0.0.1              a command line
ssh -p 2222 root@127.0.0.1 Cat \$       one command, and its status
sftp -P 2222 root@127.0.0.1             the BOX's files, through sshd's own sftp
```

## How a login works

- Keys: root's home is the host share, so `~/.ssh` is `HostFS::Host.$./ssh`. The host key `ssh_host_ed25519_key` is made there the first time. `authorized_keys` lists the keys that may log in. There are no passwords.
- The login shell is `/usr/bin/riscos-cli`, a link to `/init`. Under that name `/init` (`relay.c`) sets the terminal raw, connects to `/run/rosgd-cli.sock` and relays bytes both ways. `-c <command>` runs one command.
- Each connection is a RISC OS task (`session.c`) with its own application slot, SVC stack and environment handlers. It shows `*`, reads a line with OS_ReadLine, runs it with OS_CLI and prints any error. Quit, Exit, Logout, Ctrl-D on an empty line, or a closed connection ends it.
- VDU codes are written as ANSI. RISC OS Latin-1 is written as UTF-8, and typed UTF-8 arrives as Latin-1. A bare ESC is Escape, raised at once.
- Only one task runs at a time. A long command in one session keeps the other waiting.

The protocol between riscos-cli and `/init` starts with the line `ROSGD-CLI 1 <I|C> <cols> <rows>`. At the end `/init` sends &FF and the exit status (0, or 1 if the last command gave an error).

## Not done

- Sessions under the desktop. The Wimp's idle does not yet hand on the baton.
- Window size changes after login, per-session `*Spool` and `*Exec`.
- Reaching the BOX by an IP of its own from the Mac. QEMU's user networking gives it none.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
