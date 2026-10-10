# URL_Fetcher

A native version of RISC OS 5's URL module (0.58, `Networking/Fetchers/URL` on ROOL's GitLab). A client registers a session, asks for a URL and polls while a protocol module does the work. AcornHTTP serves http: and https:.

SWIs are at &83E00 (see `api/defs/urlfetcher.toml`): Register, GetURL, Status, ReadData, SetProxy, Stop, Deregister, ParseURL, EnumerateSchemes and EnumerateProxies. ProtocolRegister and ProtocolDeregister are at +32 and +33. The command is `*URLProtoShow`. Errors are &80DE00 + n.

- A session is a word in the RMA. It is the client's poll word. There is one fetch per session.
- GetURL canonicalises the URL, chooses a proxy, finds the protocol module for the scheme and calls its entry points (base + 0 to + 3 for GetURL, Status, ReadData and Stop).
- Protocol modules register through Service_URLModule_ProtocolModule (&83E01). Service_URLModule (&83E00) says a module started or is dying.
- `parseurl.c` is the RISC OS `c.parseurl` (Apache 2.0) with only its includes, its SWI entry and two casts changed.

Difference from RISC OS 5.30: the proxy search order is the session no-proxy list, the session proxies, the global no-proxy list, then the global proxies. The original searched the global proxies at the third step, so global no-proxy entries never took effect.

Tests: `boot/selftest_fetch.c`, hosted and in the BOX. It covers ParseURL, the schemes AcornHTTP registers, the proxy lists, and fetches over loopback.

Licence of `parseurl.c`: Apache 2.0.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
