# The BOX

**RISC OS, for Generic Devices.**

![The BOX on an ordinary PC, booted from a USB stick](screenshots/box-on-a-pc.png)

*The desktop stays live while nineteen workers draw the Mandelbrot set
on the spare cores; NetSurf on the web, a PDF open beside it.*

---

## What it is

The BOX is a **RISC OS Application Environment**. It runs where Linux
runs — on the generic devices all around us, any 64-bit PC and any
Apple silicon Mac — with RISC OS itself translated to C and compiled as
native code for the machine it is on.

Linux is the operating system: a small kernel build providing the
memory, the tasks and the devices. On top of it, the RISC OS
personality — one Linux process — creates the environment: the desktop,
the Wimp, BBC BASIC, the Filer, your applications. It is the RISC OS
you know, at the speed of the machine you already own.

## Releases

Downloads are on the [Releases page](https://github.com/albanread/Aldershot/releases):

- **BOX x64** — boots from a USB stick on a 64-bit PC.
- **BOX for Apple silicon Macs** — in a window on the Mac.
- **BOX for Intel Macs** — in a window on an Intel Mac (macOS 14 or later), signed and notarized, with its own QEMU inside.

---

## Standing on

**[RISC OS Open Ltd](https://www.riscosopen.org)** make RISC OS 5 and
publish it as open source. The system the BOX translates is theirs, as
is everything the BOX translates: no ownership is claimed over any
work translated to C. Every original author's licence is respected,
and where one asks for more, the component keeps its upstream terms.
