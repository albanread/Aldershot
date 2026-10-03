# BOX on the Network

*3 October 2026*

## Introduction

RISC OS 5 has networking: a TCP/IP stack, a resolver, Acorn Access for
file sharing, LanManFS for Windows shares, and fetchers for the web.

In BOX the network belongs to Linux. RISC OS's network modules keep
their SWIs and commands, so programs see what they always saw, but the
work underneath is done by Linux and by well-maintained libraries. This
article describes what a RISC OS program and a RISC OS user get.

---

## 1. Sockets

The Internet module is RISC OS's TCP/IP. BOX's keeps its SWI interface,
the `Socket_*` SWIs at `&41200`, and answers each with Linux's own
socket call. Linux does the rest: TCP, UDP, IPv4 and IPv6, routing and
the network driver.

What programs depend on is kept exactly:

* **Socket numbers** run from 0 to 255 and are shared across the
  machine, as on RISC OS, where one task may use a socket another
  opened.
* **Constants** are 4.4BSD's, as RISC OS's are: address families,
  socket options, flags, ioctls and error numbers. Each is translated by
  name to Linux's, never assumed equal.
* **Structures** are RISC OS's: socket addresses in both forms, 32-bit
  `timeval`s and `iovec`s, 256-bit `fd_set`s.
* **Non-blocking sockets** and `select` work as before, and **Event 19**
  (Internet event) is delivered when data arrives, so programs that
  wait for it from their `Wimp_Poll` loop carry on doing so.

### Addresses and names

When BOX starts, the Linux kernel obtains an address by DHCP itself, so
BOX needs no DHCP client of its own. The DHCP module is kept as a shim:
`*DHCPExecute`, `*DHCPInfo` and its SWIs report the kernel's lease, and
set the `Inet$` variables as RISC OS's did.

The Resolver answers `Resolver_GetHost` and the rest through Linux's
resolver, keeping RISC OS's cache and its `hostent` blocks, which
clients read directly. The name servers are the ones DHCP supplied.

### The network tools

`*Ping`, `*TraceRoute`, `*IfConfig`, `*Route`, `*InetStat`, `*ARP` and
the rest were ARM programs on RISC OS, ports of FreeBSD's. In BOX they
are commands of one native module, InetRes, answering from Linux. They
keep FreeBSD's options and output. `*MD5` gains the SHA family and the
other digests RISC OS's port left out.

---

## 2. The desktop stays responsive

RISC OS has always kept networking from freezing the desktop: the stack
works in the background, and programs use non-blocking sockets and
Event 19, or poll on null events. BOX keeps all of that, and does
better in two places:

* **Linux runs the network whatever RISC OS is doing.** Packets are
  received and TCP is handled by the kernel, not by RISC OS code at
  interrupt time.
* **Slow work goes on threads.** A TLS handshake, a name lookup or an
  HTTP transfer can run on a thread of its own, behind the same
  non-blocking SWIs, while the desktop carries on.

---

## 3. Secure connections

AcornSSL is RISC OS's TLS module. Its sixteen SWIs at `&50F80` mirror
the socket SWIs, on a TLS session. BOX's keeps the same SWIs over
**OpenSSL 3**, so programs get **TLS 1.3**, with TLS 1.2 for servers
that offer nothing newer. TLS 1.0 and 1.1 are not offered.

Certificates are checked against RISC OS's own certificate store
(`InetDBase:CertData`), and the exceptions a user has allowed are kept.

---

## 4. The web

URL_Fetcher is RISC OS's door to the web: a program registers, asks
for a URL, and reads the data as it arrives. BOX's URL_Fetcher is
native, with RISC OS's own URL parsing code kept unchanged. Its HTTP
and HTTPS module, AcornHTTP, now works over **libcurl**, with each
transfer on a thread of its own, so a fetch goes on whether or not the
program is polling. Cookies are kept by RISC OS's own code.

A BASIC program can fetch an `https:` page through URL_Fetcher, as it
could on RISC OS, but now with TLS 1.3.

---

## 5. File sharing

### Using other machines' shares

LanManFS is RISC OS's client for Windows shares. RISC OS's speaks SMB1,
which modern servers refuse. BOX's keeps LanManFS's commands and
errors, but connects with Linux's **SMB2/3** client, with whatever
signing and encryption the server asks for:

```
*LMLogon WORKGROUP alice secret
*LMConnect Docs fileserver Documents
*Cat LanMan::Docs.$
```

The share becomes a HostFS disc (see *HostFS and the Filing Systems*),
so it can also be reached as `HostFS::Docs.$`.

### Sharing the box's files

RISC OS shares its own files with Acorn Access, a protocol only RISC
OS speaks. BOX shares them over **SMB3** instead, using ksmbd, the SMB
server built into Linux, so Macs, Windows and Linux machines can all
connect. The commands keep Access's shape:

```
*SMBUser alice secret
*Share HostFS::Host.$.Public
*Share $.Projects Work -readonly
*Shares
```

A Mac connects from the Finder with Go > Connect to Server, using an
`smb://` address.

---

## 6. SSH

RISC OS has never had SSH. BOX has both ends of it, from OpenSSH.

* **The client.** `*SSH` runs OpenSSH's `ssh` on a pseudo-terminal, and
  draws its output in the RISC OS text window, so you can log in to
  another machine from the `*` prompt. `*SSH-KeyGen` makes keys.
* **The server.** `*SSHD` starts OpenSSH's server. A login is a RISC OS
  command line, not a Linux shell: the `*` prompt, with `*` commands,
  BASIC and the rest. A single command can be run directly, and `sftp`
  reaches the box's files.

When the Intel box runs on a Mac, its launcher forwards the Mac's port
2222 to the box's SSH server:

```
ssh -p 2222 root@127.0.0.1              a RISC OS command line
ssh -p 2222 root@127.0.0.1 Cat \$       one command
```

Keys are kept in `.ssh` on the host share. There are no passwords; only
keys listed in `authorized_keys` may log in.

---

## 7. What is not carried

* **Acorn Access and Econet.** Access is replaced by SMB3, as above.
  Econet, AUN and the old network drivers are not carried.
* **RISC OS's own network drivers and stack**, which Linux replaces.
* **NFS** is planned, over Linux's client and server, but not yet done.
