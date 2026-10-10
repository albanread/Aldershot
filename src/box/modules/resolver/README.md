# Resolver

The Resolver is RISC OS's name lookup service. This is a native C module
(`resolver.c`) that answers the four SWIs of chunk &46000 and uses the host's
own resolver underneath.

RISC OS 5 ships the Resolver (0.76, May 2025) only as a binary,
`Networking/ResolverBlob/aof/Resolver`, with no source. The behaviour here
was read from that binary's disassembly. One difference from the old ANT
documentation: in RISC OS 5 the name for `GetHost` is in R0, and R0 = 0
selects a lookup by address.

## Use

The SWIs are typed in `api/defs/resolver.toml`.

| SWI | In | Out |
| --- | --- | --- |
| `Resolver_GetHostByName` &46000 | R1 name | R0 status, R1 hostent or 0 |
| `Resolver_GetHost` &46001 | R0 name, or R0 = 0 and R1 a 4 byte address | R0 status, R1 hostent or 0 |
| `Resolver_GetCache` &46002 | R0 entry, or 0 for the first | planned |
| `Resolver_CacheControl` &46003 | R0 reason | none |

- Status 0 means the host was found. The hostent is five words in the RMA:
  name, alias list (empty), address type (2), address length (4), and
  address list. It is IPv4 only, and valid until the cache is flushed.
- `GetHostByName` blocks until the lookup ends. Escape ends the wait in a
  task window.
- `GetHost` with a name does not block. The first call for a name that
  needs DNS starts the lookup and returns 36 (`EINPROGRESS`). The caller
  calls again until the status is 0, or a failure status with R1 = 0.
- `GetHost` with R0 = 0 looks up an address through its `in-addr.arpa`
  name, and blocks.
- A name that does not exist gives status 1 (`HOST_NOT_FOUND`). A server
  that did not answer gives 2 (`TRY_AGAIN`).
- Errors are &818040 plus: 0 "Bad parameters to Resolver SWI" (an empty
  name, or a character other than letter, digit, `-`, `.` or `_`), 2 "No
  DNS service configured", 6 "No free memory for Resolver".
- Dotted addresses and names in `/etc/hosts` are answered at once. Names
  looked up are cached, case insensitively.
- `CacheControl` reasons 0 to 2 flush the cache. Reason 3 is accepted.
  Reasons 4 to 9 are accepted and ignored.

## Differences from RISC OS 5.30 and what is not done

- Name servers come from `/etc/resolv.conf`, which the kernel's DHCP
  provides. RISC OS uses `Inet$Resolvers`. The hosts file is `/etc/hosts`,
  not `InetDBase:Hosts`.
- `GetCache` is not done. The meaning of three of its fields was not
  settled.
- `CacheControl` flushes every entry whatever the reason's masks say,
  except lookups still in progress.
- The original's status numbers for a failed lookup are not known. Its
  clients test only zero against non-zero.
- With no name server configured the error comes at once, not after a
  timeout.

## Tests

`boot/selftest_resolver.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
