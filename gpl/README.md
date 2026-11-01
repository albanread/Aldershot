# Source for BOX's copyleft components

BOX contains programs under the GNU GPL and LGPL, the MPL 2.0 and the CDDL. This folder holds their source: BOX's own ports, patches, build scripts and kernel configurations, with the upstream version each starts from. Unmodified upstream code is named, with where to get it, and not copied.

All of it is from the BOX sources at commit `2bd8aa9b8e`, from which the two Mac releases of 18 October 2026 (2026.10.18.4) were built (nothing here differs from `5f146e0451`, which built 2026.10.18); since `fe54aa91ca` the Linux folder has gained the kernel's backings-region driver and the Mac kernels' new configurations, and nothing else here has changed. The PC release of 11 October 2026 (2026.10.11.3) and its kernel configuration are as before, from `fe54aa91ca`; this folder as it was for it is Aldershot commit `acd3aff`'s. Since the earlier 11 October releases (`7f84f3415d` and `abea3f9628`, this folder as Aldershot commit `ba6ee93` has it) nothing here has changed: 2026.10.11.3's fixes are in BOX's own MIT code. The releases of 8 October were built from `a120e1f166`, `71171be1dc` and `78b8e0ad38`; this folder as it was for them is Aldershot commit `e42fc30`'s. The Intel Mac and PC releases of 7 October were built from `e8c826a19f`; this folder as it was for them is Aldershot commit `e4fe999`. BOX's own code is under the MIT licence. Each component keeps its own licence; see [LICENSES](../LICENSES/README.md).

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
| [box-micropolis](https://github.com/albanread/box-micropolis) | Micropolis, the city simulation !Micropolis runs, with BOX's changes and its engine (on main since the releases; not yet in one) | GNU GPL 3, with Electronic Arts' additional terms |
| [unmodified](unmodified/) | glibc, GCC runtime, FreeFont, Debian packages | GNU GPL / LGPL |
