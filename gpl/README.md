# Source for BOX's copyleft components

BOX contains programs under the GNU GPL and LGPL, the MPL 2.0 and the CDDL. This folder holds their source: BOX's own ports, patches, build scripts and kernel configurations, with the upstream version each starts from. Unmodified upstream code is named, with where to get it, and not copied.

All of it is from the BOX sources at commit `a120e1f166`, which built the Apple silicon releases of 8 October 2026 (with `1ebcab5ff2` and `71171be1dc`, whose changes since are BOX's own MIT code). The Intel Mac and PC releases of 7 October were built from `e8c826a19f`; this folder as it was for them is Aldershot commit `e4fe999`. BOX's own code is under the MIT licence. Each component keeps its own licence; see [LICENSES](../LICENSES/README.md).

| Folder | Component | Licence |
| --- | --- | --- |
| [linux](linux/) | Linux kernel 6.18.54 (configuration) | GNU GPL 2 |
| [netsurf](netsurf/) | NetSurf | GNU GPL 2; its libraries MIT |
| [ksmbd-tools](ksmbd-tools/) | ksmbd-tools, GLib, libnl | GNU GPL 2; GNU LGPL 2.1 |
| [paige](paige/) | Paige and !Write | GNU LGPL 2.1 |
| [pipedream](pipedream/) | PipeDream 4.63 | MPL 2.0 |
| [sparkfs](sparkfs/) | SparkFS | CDDL 1.0 and Apache 2.0 |
| [wpe-edition](wpe-edition/) | Web browser programs | MIT; libraries: see [LICENSES/wpe](../LICENSES/wpe/) |
| [qemu](qemu/) | QEMU | GNU GPL 2 |
| [unmodified](unmodified/) | glibc, GCC runtime, FreeFont, Debian packages | GNU GPL / LGPL |
