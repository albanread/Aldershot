# AcornSSL

A native module that gives TLS a socket-shaped interface, following ROOL's
AcornSSL 1.09 (`Networking/Fetchers/AcornSSL`). It uses OpenSSL 3 where the
original uses mbedTLS, so it offers TLS 1.3 and TLS 1.2. TLS 1.0 and 1.1 are
not offered.

## SWIs

Base &50F80: Creat, Ioctl, Connect, Shutdown, Close, Getsockopt, Write, Recv,
CreateSession, Getpeername, Getsockname, Setsockopt, Stat, Version, Read, Send.

- The socket belongs to the Internet module. Creat makes one, and
  CreateSession adopts the caller's.
- The handshake happens on the first Send or Recv.
- Options: SO_ACORNSSL_HOSTNAME (&11E0) is the name the certificate must match
  and is sent as SNI. SO_ACORNSSL_PROMPTTIME (&11E1) is how long the user may
  take to answer a certificate prompt (30 seconds by default, 0 rejects at
  once).
- Errors are &813F20 + n, with the original's messages.
- The module issues Service_URLModule_SSL (&83E02) when it starts and dies.

## Differences from RISC OS 5.30

- Certificates are checked against `InetDBase:CertData`, or the copy in the
  ROM. A chain that fails is offered to UpCall_CertificateConfirm (26). There
  is no AcornSSL desktop task yet, so an unclaimed chain is rejected.
- OpenSSL 3.6.4 is built static for musl by `deps/build-openssl.sh`.

## Tests

`boot/selftest_acornssl.c`, hosted and in the box, against a loopback TLS 1.3
server. It covers the handshake, the certificate prompt, non-blocking I/O,
MSG_PEEK and the options.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
