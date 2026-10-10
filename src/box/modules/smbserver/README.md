# SMBServer

Shares directories of the BOX over SMB2/3, so that Macs, Windows and Linux can mount them. RISC OS's own sharing (Acorn Access, ShareFS) is not supported. The module drives ksmbd, the Linux in-kernel SMB server, through ksmbd-tools.

| Command | Meaning |
| --- | --- |
| `*Share <directory> [<name>] [-readonly] [-guest [-write]]` | Shares a HostFS or LanMan directory. The name defaults to the directory's leaf. `-guest` lets anyone in without a password, and is read only unless `-write` is also given. |
| `*Shares` | Lists the shares. |
| `*UnShare <name>` | Stops sharing. |
| `*SMBUser <user> [<password>]` | Adds or changes a user. With no password it removes the user. |

ksmbd.mountd starts with the first share. Each change is written to `/etc/ksmbd/ksmbd.conf` and passed on with `ksmbd.control --reload`. Users are kept in `ksmbdpwd.db` beside it. The server's messages go to `/run/ksmbd.log`.

ksmbd-tools 3.5.7 is one static program, `/usr/sbin/ksmbd.tools`, run under the names ksmbd.mountd, ksmbd.adduser and ksmbd.control. `deps/build-ksmbd.sh` builds it.

From the Mac, `run/run-x86_64.sh` forwards 127.0.0.1:1445 to port 445 of the BOX. In the Finder use Go > Connect to Server with `smb://<user>@127.0.0.1:1445/<share>`.

What the server allows:

- Signing is required (`server signing = mandatory`). Clients that cannot sign cannot connect.
- It listens on the loopback and the interfaces that are up when it starts. `SMBServer$Interfaces` sets a list instead. A change takes effect when the server next starts.
- Files are served as root, so a share can be any HostFS directory. `.ssh` is never served.
- Symbolic links are not followed. Directory and share names that `ksmbd.conf` cannot hold are refused.
- The configuration, the password database and their directory are readable by root only.
- Adding the first guest share to a running server restarts it.

Not done: users and shares are not kept from one boot to the next (use an Obey file). DOS attributes are not stored. Printers are not shared.

Tests: `tests/smb/mac-vz.sh` does the same on the Apple silicon box and also checks wrong passwords, guest access, `.ssh` and the interface list. `tests/smb/mac.sh` boots the BOX headless, adds a user and a share over SSH, mounts the share with macOS's own client, lists, reads and writes it, and shows the dialect, signing and encryption.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
