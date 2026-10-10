# AcornHTTP

A native module that provides the http: and https: protocols for URL_Fetcher.
It follows ROOL's AcornHTTP 1.09 (`Networking/Fetchers/HTTP`) and uses
libcurl for the transfers. Each transfer runs on a thread of the module's own,
so a fetch goes on whether or not the client polls.

## SWIs

| SWI base | Calls |
|---|---|
| &83F80 (http:) | GetData, Status, ReadData, Stop |
| &83F90 (https:) | the same four |
| &83FBB, &83FBC | RegisterMethod, DeregisterMethod |
| &83FBD to &83FBF | AddCookie, ConsumeCookie, EnumerateCookies |

The module registers http: and https: with URL_Fetcher at start and again
when URL_Fetcher restarts. The session is the client's poll word (R1). The
status codes and error numbers (&80DE20 + n) are the original's.

## Differences from RISC OS 5.30

- libcurl 8.22.0 (static, built by `deps/build-curl.sh`) does the transfer,
  so https: uses OpenSSL 3 and TLS 1.3.
- Certificates are checked against InetDBase:CertData or the ROM's copy.
  A bad chain fails the fetch. The desktop prompt that AcornSSL offers is not
  yet used here.
- `cookie.c` is AcornHTTP's own cookie jar, ported to use FileSwitch and
  system variables. `dates.c` is the original's date parser.

## Tests

`boot/selftest_fetch.c`, hosted and in the box, against a loopback HTTP
server. It covers chunked and Content-Length responses, cookies, POST, a
registered method, redirects, HEAD and the error cases.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
