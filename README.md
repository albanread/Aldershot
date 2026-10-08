# BOX

RISC OS for generic devices: RISC OS translated to C, running on a Linux kernel, on a 64-bit PC or a Mac.

![BOX on a PC, booted from a USB stick](screenshots/box-on-a-pc.png)

## Downloads

Early previews, from the [Releases page](https://github.com/albanread/Aldershot/releases):

- [BOX for Apple silicon Macs](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026.10.08) (macOS 14 or later)
- [BOX for Intel Macs](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-mac-2026.10.07) (macOS 14 or later)
- [BOX x64 for PCs](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-pc-2026.10.07) (USB stick image, UEFI)

## Ports

The modules and applications in BOX are ported. Some need only small changes. Others are rewritten completely in C. The work is done a step at a time, and a port may bring in bugs that the original does not have.

Please do not report missing features or faults in BOX's software to the original authors. Their ARM code very likely works perfectly on a Raspberry Pi 4; the fault is in the port, not in their work.

There is no need to report them here either. The project finds faults itself, by testing BOX against RISC OS 5.30.

## Licences

- [LICENSES](LICENSES/README.md): the licence of every component. BOX's own code is under the MIT licence. RISC OS is RISC OS Open Ltd's, under the Apache License 2.0.
- [gpl](gpl/README.md): the source of the components under the GNU GPL, GNU LGPL, MPL 2.0 and CDDL.

BOX claims no rights over code it has translated. Every original author's licence applies to their work.
