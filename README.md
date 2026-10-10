# BOX

A RISC OS environment for generic devices.

*RISC OS's components translated to C, running on a Linux kernel, on a 64-bit PC or a Mac.*

![BOX on a PC, booted from a USB stick](screenshots/box-on-a-pc.png)

## Downloads

Early previews, from the [Releases page](https://github.com/albanread/Aldershot/releases):

- [BOX for Apple silicon Macs](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026.10.08.3) (macOS 14 or later)
- [BOX for Intel Macs](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-mac-2026.10.08) (macOS 14 or later)
- [BOX x64 for PCs](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-pc-2026.10.08) (USB stick image, UEFI)
- [Just BBC BASIC translated](https://github.com/albanread/Aldershot/releases/tag/BBCBASICVA64-1.2) (BBCBASICVA64, Apple silicon Macs, macOS 14 or later)
- [Just BBC BASIC translated, for Intel Macs](https://github.com/albanread/Aldershot/releases/tag/BBCBASICVX64-1.2) (BBCBASICVX64, macOS 14 or later)

## Ports

The modules and applications in BOX are ported. Some need only small changes. Others are rewritten completely in C. The work is done a step at a time, and a port may bring in bugs that the original does not have.

Please do not report missing features or faults in BOX's software to the original authors. Their ARM code very likely works perfectly on a Raspberry Pi 4; the fault is in the port, not in their work.

There is no need to report them here either. The project finds faults itself, by testing BOX against RISC OS 5.30.

## Performance

RISC OS's own ARM code is extremely efficient on an ARM processor such as the Raspberry Pi 4's Cortex-A72. Porting that code to C costs a good deal of speed, and needs more memory. Translated C is also 1.5 to 3 times slower than C written by hand.

So BOX is not suited to Raspberry Pi class machines. It performs well on computers such as Apple silicon Macs, or PCs with an Intel Core i7-12700.

BBC BASIC suffers from the translation too, and does not run as much faster as the difference in processor power might suggest. To make up for this, BOX includes a translator from BBC BASIC to C, `*RosBas`.

## Documents

- [articles](articles/README.md): technical articles about BOX, in the style of the
  RISC OS Programmer's Reference Manuals. Start with [BOX Design](articles/0-Design.md).
- [docs](docs/README.md): documents about BOX. [Bugs and quirks](docs/bugs-and-quirks.md) lists the bugs, quirks and documentation errors noticed in RISC OS 5.30 and its applications while porting them.

## Licences

Important licence update information has been added to the repository: see [LICENSES](LICENSES/README.md).

*Anyone with genuine concerns about software licences should raise an issue.*

*BOX does not claim ownership of any code that the project did not originally write. The project is publishing translated source code: the C in [src/box](src/box/README.md) is translated from other people's code, and keeps their licence. The project actually builds most modules from the original assembler code, which is typically more readable, and which is available from its own repository ([RISC OS Open's](https://gitlab.riscosopen.org/RiscOS/Sources)).*

- [LICENSES](LICENSES/README.md): the licence of every component. BOX's own code is under the MIT licence. RISC OS is RISC OS Open Ltd's, under the Apache License 2.0.
- [gpl](gpl/README.md): the source of the components under the GNU GPL, GNU LGPL, MPL 2.0 and CDDL.

Code that BOX has translated to C stays under the licence of the code it was translated from: RISC OS translated to C is still under the Apache License 2.0. BOX claims no ownership of translated code.

## Source code

The project is publishing translated source code: the C in [src/box](src/box/README.md) is translated from other people's code, and keeps their licence. The project actually builds most modules from the original assembler code, which is typically more readable, and which is available from its own repository ([RISC OS Open's](https://gitlab.riscosopen.org/RiscOS/Sources)).

The source of every component whose licence asks for it is in [gpl](gpl/README.md).

Published so far, in [src](src):

- [BOX](src/box/README.md), the source of `/init`: the runtime, the platform layer, the boot code and the modules written for BOX;
- [ROSASM](src/ROSASM/README.md), the ObjAsm-compatible assembler;
- [ROSBAS](src/ROSBAS/README.md), the BBC BASIC V compiler (the compiler only; its runtime library will follow).
- [BBC BASIC V, translated to C](src/translated/bbcbasic/README.md): RISC OS's BBC BASIC interpreter, translated by hand from ARM assembler; under ROOL's Apache License 2.0.
