# Source code of the BOX's copyleft components

The BOX releases (the Mac apps and the PC image of 7 October 2026) contain
programs under the GNU GPL and LGPL, and under the Mozilla Public License and the
CDDL, which also ask for their source. This folder is that source: for each
component, the BOX's own code that builds it -- its port, patches, build scripts
and, for the kernel, its configuration -- together with the exact upstream
version it starts from. Upstream code that the BOX uses unchanged is named,
with where to get it, rather than copied.

Everything here is from the BOX's sources at commit `e8c826a19f`, the state the 7 October
2026 releases were built from. The BOX's own code is under the MIT licence; each
component keeps its own licence, given in its folder and in
[../LICENSES](../LICENSES/README.md).

| Folder | Component | Licence | What is here |
| --- | --- | --- | --- |
| [linux](linux/) | the Linux kernel 6.18.54 | GNU GPL 2 | the configurations (fragments and the full `.config` of each kernel), the build scripts; the kernel source itself is unchanged |
| [netsurf](netsurf/) | NetSurf, the web browser !NetSurf | GNU GPL 2 (NetSurf), MIT (its libraries) | the BOX's front end, build script and test tools, the POSIX layer it is linked with, and the upstream commits |
| [ksmbd-tools](ksmbd-tools/) | ksmbd-tools, with GLib and libnl | GNU GPL 2; GNU LGPL 2.1 | the build script; the upstream releases unchanged |
| [paige](paige/) | Paige, the text engine of !Write | GNU LGPL 2.1 | the patch to HERMES-Paige, the platform layer, the build script, and !Write's own source (it is linked with Paige) |
| [pipedream](pipedream/) | PipeDream 4.63 | Mozilla Public License 2.0 | the whole program as the BOX builds it |
| [sparkfs](sparkfs/) | SparkFS 1.50+ | CDDL 1.0 and Apache 2.0, file by file | the BOX's port, patches and build script, and the upstream commits |
| [wpe-edition](wpe-edition/) | the web browser's programs | MIT (the BOX's); the libraries they use: see [../LICENSES/wpe](../LICENSES/wpe/) | the compositor, the browser's engine, wperun and the root's builder |
| [qemu](qemu/) | QEMU, in the Mac apps | GNU GPL 2 | where its source is |
| [unmodified](unmodified/) | glibc, the GCC runtime, FreeFont, the WPE root's Debian packages | GNU GPL / LGPL | where their source is |
