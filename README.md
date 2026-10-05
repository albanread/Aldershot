# The BOX

**RISC OS, for Generic Devices.**

![The BOX on an ordinary PC, booted from a USB stick](screenshots/box-on-a-pc.png)

*The desktop stays live while nineteen workers draw the Mandelbrot set
on the spare cores; NetSurf on the web, a PDF open beside it.*

---

## What it is

The BOX is a **RISC OS Application Environment**. It runs where Linux
runs — on the generic devices all around us, any 64-bit PC and any
Mac, Apple silicon or Intel — with RISC OS itself translated to C and compiled as
native code for the machine it is on.

Linux is the operating system: a small kernel build providing the
memory, the tasks and the devices. On top of it, the RISC OS
personality — one Linux process — creates the environment: the desktop,
the Wimp, BBC BASIC, the Filer, your applications. It is the RISC OS
you know, at the speed of the machine you already own.

## Releases

Downloads are on the [Releases page](https://github.com/albanread/Aldershot/releases).

### The BOX on a Mac

Two releases of 5 October 2026, one for each kind of Mac. Each is a disk
image holding `BOX.app`, signed with a Developer ID and notarized by
Apple. BOX has its own QEMU inside and uses the Mac's own hypervisor, so
nothing is emulated and nothing else needs installing: no Homebrew, no
Xcode. Both are early-days previews.

| Release | For | Download |
| --- | --- | --- |
| [BOX for Apple silicon Macs](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026.10.05) | macOS 14 or later, M1 or later | `BOX-arm64-mac-2026.10.05.dmg`, 170 MB |
| [BOX for Intel Macs](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-mac-2026.10.05) | macOS 14 or later, an Intel Mac | `BOX-x64-mac-2026.10.05.dmg`, 174 MB |

* **Installing.** Open the disk image, drag BOX to Applications, and
  open it.
* **Your RISC OS disc.** The first time, BOX makes the folder
  "RISC OS BOX" in your home folder. In RISC OS it is the Host disc on
  the icon bar, and what you save there stays there. Hold Option as BOX
  starts to choose another folder.
* **Updating.** Drop the new BOX over the old one. The system — the
  kernel, the ROM and QEMU — is inside the app; BOX brings the programs
  on your disc up to date the first time it runs, and never touches what
  you saved.
* **Settings**, from Terminal, for example `open -a BOX --env BOX_CPUS=8`:
  `BOX_CPUS`, `BOX_MEMORY`, `BOX_SIZE` (the desktop in RISC OS pixels),
  `BOX_SCALE`, `BOX_SSH=on` (SSH on port 2222, for your own key), and on
  Apple silicon `BOX_SOUND=off`. The console is kept in
  `~/Library/Logs/RISC OS BOX/console.log`.

What comes with it: the RISC OS desktop and its applications, BBC BASIC,
NetSurf, StrongED, PipeDream, OvationPro and the ArtWorks viewer, !PDF,
Write, a C compiler (clang) and Mojo. The licence of every component is
in `Documents.Licences` on the disc.

### The BOX on a PC

**BOX x64** boots a 64-bit PC from a USB stick, UEFI with Secure Boot
off.

---

## Standing on

**[RISC OS Open Ltd](https://www.riscosopen.org)** make RISC OS 5 and
publish it as open source. The system the BOX translates is theirs, as
is everything the BOX translates: no ownership is claimed over any
work translated to C. Every original author's licence is respected,
and where one asks for more, the component keeps its upstream terms.
