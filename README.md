# Aldershot

**RISC OS, on the computer you already own.**

> ⚠️ **This is brand-new software.** It's early days: it works and it's
> usable today, but you may meet the odd rough edge. If something doesn't
> behave, please let us know.

This is the front door. It is where you find out what exists, whether it
works yet, and where to get it. There is no source code in this
repository — just these pages, the user guide, the developer
walkthroughs and case notes, and the downloads on the
[Releases](https://github.com/albanread/Aldershot/releases) page.

Something is listed here when it actually runs, not when it is started.

---

## Two ways to run RISC OS

There are two of them, and they are different kinds of thing, so each has
its own page:

| | | |
| --- | --- | --- |
| 🥧 | **[RISC OS emulation](emulation/README.md)** | The genuine RISC OS, unmodified, on an emulated Raspberry Pi. **Works today** on the Mac and Windows. |
| 🚀 | **[RISC OS translation — BOX](translation/README.md)** | **RISC OS, on a Linux kernel, translated to C** — native code, no emulator. Early-days preview: booting from a USB stick on a 64-bit PC, or in a window on an Apple silicon Mac. |

**Emulation** runs the real thing. A program on your computer pretends to
be a Raspberry Pi — the machine RISC OS is built for these days — and
RISC OS 5.30, exactly as [RISC OS Open](https://www.riscosopen.org)
publish it, boots inside the pretence. Nothing about RISC OS changes:
you open the app, and the familiar desktop appears. The Mac and Windows
editions work this way, and they are the finished-feeling ones today.

**Translation** rebuilds RISC OS itself. The system is translated to C
and compiled to native code for your processor, with a small Linux
kernel underneath doing what a kernel does — memory, tasks, devices. No
machine is emulated; the desktop runs on the metal of your computer.
That is BOX, and it is young: a preview, offered early, for people who
like being close to the machinery. It is where this project is going.

Both give you the same RISC OS — the same desktop, the same applications
— and both carry the host's conveniences: your files shared as a plain
folder, your network, your speakers. They even carry **different
licences**, because they are different kinds of work: the emulator is
GPL, like QEMU, which it is built from; BOX is Apache 2.0 / MIT, the
system having been translated. Each page says which, and where its
source lives.

Not sure which you want? Use the emulator today; watch BOX.

---

## What the emulator looks like

![The RISC OS desktop, running on the Mac](screenshots/final-desktop.png)

The RISC OS desktop, running in the Mac edition: booted from **HostFS**,
its disc a folder on the Mac, with **HostNet** passing its networking to
the Mac's.

And BOX, the translated edition, on an ordinary PC:

![RISC OS BOX on my HP i7-12700 with NV T1000 graphics, booted from USB](screenshots/box-on-a-pc.png)

RISC OS BOX on an HP i7-12700 with NVIDIA T1000 graphics, booted from USB —
the Mandelbrot set drawn by nineteen workers on the spare cores while the
desktop stays live.

---

## For developers

The [developer walkthroughs](walkthroughs/README.md) explain how all of
this was built, for anyone who wants to understand it or work on it:

- [RISC OS on a Pi 4, in QEMU](walkthroughs/QemuA72Walkthrough/index.md) — start here
- [Fake it in software](walkthroughs/FakeItInSoftwareWalkthrough/index.md)
- [Graphics and sound, done by the host](walkthroughs/GraphicsSoundWalkthrough/index.md)
- [The backdrop layer: a background the host draws beneath RISC OS](walkthroughs/BackdropWalkthrough/index.md)
- [HostFS: a host directory as a RISC OS disc](walkthroughs/HostFSWalkthrough/index.md)
- [HostNet: the guest's sockets, served by the host](walkthroughs/HostNetWalkthrough/index.md)
- [Mojo for RISC OS](walkthroughs/MojoRISCOSWalkthrough/index.md)

Each one is also a single PDF.

The [case notes](case-notes/README.md) file problems met while running
real software, with the evidence and what is still open. The first is
[WimpForth: an fsave that aborts the machine](case-notes/2026-09-15-wimpforth-fsave-abort/index.md).

---

## The small print

- **[RISC OS emulation](emulation/README.md)** — the Mac and Windows
  applications are open source under the
  [GNU General Public License, version 2](LICENSE), the licence of the
  QEMU they are built from; their source is
  [RISCOSQEMUA72](https://github.com/albanread/RISCOSQEMUA72). The RISC
  OS inside them is RISC OS Open's, Apache 2.0.
- **[RISC OS translation — BOX](translation/README.md)** — Apache 2.0 /
  MIT, respecting the licences of the original authors of the translated
  software. Source and documentation follow on; they are still being
  written.

Details, and what each licence covers, are on each edition's page.

## Standing on

**[QEMU](https://www.qemu.org)** does the hard part of the emulated
editions — pretending to be a Raspberry Pi convincingly enough that
RISC OS never notices. **[RISC OS Open Ltd](https://www.riscosopen.org)**
make RISC OS 5 and publish it as open source; both editions are their
system, one running it, one translating it. Our thanks to both.
