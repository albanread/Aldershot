# Internet

RISC OS 5's Internet module is a 4.4BSD TCP/IP stack with a SWI interface.
Here the stack is Linux's, so only the interface is written: a native
module in C (`internet.c`) that answers the `Socket_*` SWIs with the host's
socket calls. Names, numbers and layouts are RISC OS's. The sources it
follows are the original's cmhg header, `riscos/c/socket_swi` and the
TCPIPLibs headers.

Linux does TCP, UDP, IPv4 and IPv6, routing and the network driver. With
`ip=dhcp` on the kernel command line the kernel runs DHCP itself, and
`/init` links `/etc/resolv.conf` to `/proc/net/pnp` (`platform/net_linux.c`).
The lease is not renewed. Name lookup is the Resolver's
(`modules/resolver`).

## Use

The SWIs are in chunk &41200 and are defined in `api/defs/internet.toml`.
Each is the BSD call of its name, with arguments in R0 upwards and the
result in R0. They are `Creat`, `Bind`, `Listen`, `Accept`, `Connect`,
`Recv`, `Recvfrom`, `Recvmsg`, `Send`, `Sendto`, `Sendmsg`, `Shutdown`,
`Setsockopt`, `Getsockopt`, `Getpeername`, `Getsockname`, `Close`,
`Select`, `Ioctl`, `Read`, `Write`, `Readv`, `Writev`, `Gettsize` (256),
`Version` (568), and the 4.4 forms `Accept_1`, `Recvfrom_1`, `Recvmsg_1`,
`Sendmsg_1`, `Getpeername_1` and `Getsockname_1`.

- Socket numbers are 0 to 255 and machine wide, as on RISC OS.
- Errors are &20E00 plus the 4.4BSD errno. The message is the host's
  `strerror()`.
- Both sockaddr forms are accepted. The unsuffixed calls return the 4.3
  form and the `_1` calls the 4.4 form. `AF_INET6` is 28, as in BSD.
- `Ioctl` supports `FIONBIO`, `FIONREAD`, `SIOCATMARK`, `FIOASYNC` and the
  read only interface requests (`SIOCGIFCONF`, `SIOCGIFFLAGS` and so on).
  Setting interfaces or routes gives `EOPNOTSUPP`, because Linux
  configures them.
- A call that waits releases the personality lock while it waits, so
  background work goes on.
- A socket with `FIOASYNC` raises the Internet event (event 19) when
  input arrives, with R1 = 1 (2 for urgent data, 3 for a broken
  connection), R2 the socket and R3 its local port. After an event, the
  next one is raised only when the input has been read.

## Differences from RISC OS 5.30 and what is not done

- `Stat`, `Sendtosm`, `Sysctl` and `InternalLookup` are planned. Calling
  them gives "SWI out of range".
- Access rights and control data in a msghdr are not carried.
- BSD raised SIGIO for every arrival. Here there is one event for each
  read, so a slow client is not flooded.
- Interfaces and routes cannot be set from RISC OS.

## Tests

`boot/selftest_internet.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
