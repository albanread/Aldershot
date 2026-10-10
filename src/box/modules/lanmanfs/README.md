# LanManFS

LanManFS gives access to SMB2 and SMB3 shares. RISC OS 5's LanManFS
(`Networking/Omni/Protocols/OmniLanManFS`) speaks SMB1 itself, which
current Samba and Windows refuse. This one is a native C module
(`lanmanfs.c`) that uses libsmb2, a client in user space (built by
`deps/build-libsmb2.sh`). It negotiates signing and encryption as the
server asks. Each share is a HostFS disc whose calls are this module's.

## Use

```
*LMLogon WORKGROUP alice secret
*LMConnect Docs fileserver Documents
*Cat LanMan::Docs.$
*LMDisconnect Docs
```

- `*LMConnect <name> <server> <share> [<user>|- [<password>]]` mounts a
  share as disc `<name>`. With no user it uses `*LMLogon`'s, and with none
  of those it connects as a guest.
- `*LMDisconnect <name>` unmounts it.
- `*LMLogon <workgroup> <user> [<password>]` sets the defaults, and
  `*LMLogoff` clears them.
- `*LMInfo` lists the connections. `*LanMan` selects the filing system.
- The filing system LanMan (number 102) reaches the same discs as HostFS,
  so `LanMan::Name.$` paths work. Its `$` is the first connection's.
- Errors are LanManFS's own, &16600 plus a number, with its messages.
- Servers are host names or addresses, looked up as the Resolver does.

SMB keeps no RISC OS attributes. A file is locked when the server has it
read only, and locking or unlocking it sets that. A connection that the
server has dropped is made again at the next call, and files open on it
are lost.

## Differences from RISC OS 5.30 and what is not done

NetBIOS and SMB1 are not spoken. These are not carried: NetBIOS names and
browsing (`*ListFS`, `*LMServer`, `*LMPrinters`, `*Configure
LMNameServer`), DOS short name casing (`*LMNameMode`), and OmniClient's
SWIs (&49240). OmniClient is an ARM Wimp application.

## Tests

`boot/selftest_smb.c` checks the commands, the errors and the filing system name. In the box it also connects to a share served by ksmbd over the loopback.

libsmb2 is Ronnie Sahlberg's, under the LGPL 2.1.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
