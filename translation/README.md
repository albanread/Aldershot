# RISC OS translation — BOX

![RISC OS BOX on my HP i7-12700 with NV T1000 graphics, booted from USB](../screenshots/box-on-a-pc.png)

*RISC OS BOX on my HP i7-12700 with NV T1000 graphics, booted from USB.*
The Mandelbrot set drawn by nineteen workers on the spare cores while the
desktop stays live, NetSurf on the web, a PDF open, and nothing under it
but its own Linux — started from a USB stick on an ordinary PC.

**RISC OS, on a Linux kernel, translated to C.**

The [emulated editions](../emulation/README.md) run the real RISC OS inside
a program that pretends to be a Raspberry Pi. BOX is the other way round:
**the RISC OS system itself is translated to C and compiled as native code
for your processor.** No machine is emulated, no Pi is pretended — a small
Linux kernel provides the hardware's kernel services (memory, tasks,
devices), and a **RISC OS personality** runs on top of it as native x64
code.

Same desktop, same BASIC, same Wimp, same applications — but the work is
done by your computer, not by a simulation of someone else's. That is the
promise of translation: the speed and integration of native software, with
the RISC OS you know.

*This is high atmospheric testing compared to the moonshot, but we aim
for low earth orbit.*

How it works is described in the [BOX articles](../articles/README.md).
The first is **[BOX Architecture](../articles/box-architecture.md)**: the
kernel, the personality, the arena, tasks as threads, and the SWI path.

---

## What runs today

The first **preview releases of BOX**, on 3 October 2026 (see
[Downloads](#downloads)), with a few applications to try — for people who
like being close to the machinery. Not emulated. Native.

- **BOX x64** boots RISC OS from a **USB stick on a 64-bit PC**.
- **BOX for Apple silicon Macs** runs RISC OS, compiled for the Mac's own
  processor, in a window on the Mac.

### One thing to know: memory

Native x64 code uses a lot more memory than the 32-bit RISC OS you may
be used to — compiled code, its runtime, everything, is bigger and
happier with bigger buffers (the pointers stay 32-bit, in the BOX way).
The desktop still starts with the slots you know, but **adjust your
WimpSlots accordingly**: give tasks more than their classic values, and
expect the next slot's default to be larger. An application that ran
happily in 640K of emulation wants more here.

### Where it fits

This is the same RISC OS as the Mac and Windows releases — the same
BASIC, the same Wimp, the same desktop — running on the metal of your
PC, Linux underneath doing what a kernel does. It is where this project
is going, offered early — for the joy of it.

### What it looks like

The picture at the top of this page: BOX on an HP i7-12700, booted from a
USB stick, drawing the Mandelbrot set on nineteen cores with the desktop
still answering, NetSurf on the web and a PDF open beside it.

---

## Downloads

| Your computer | Download |
| --- | --- |
| **A 64-bit PC** (or a virtual machine on one) | [BOX x64, 3 October 2026](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-2026-10-03): below |
| **An Apple silicon Mac** (M1 or later) | [BOX for Apple silicon Macs, 3 October 2026](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026-10-03): see [BOX on an Apple silicon Mac](#box-on-an-apple-silicon-mac) |

### BOX x64, for PCs

| Download | What it is |
| --- | --- |
| [BOX-x64-2026-10-03.img.xz](https://github.com/albanread/Aldershot/releases/download/BOX-x64-2026-10-03/BOX-x64-2026-10-03.img.xz) | the USB stick image, compressed (41 MB; 8.7 GB unpacked) |
| [BOX-x64-2026-10-03-vmware.zip](https://github.com/albanread/Aldershot/releases/download/BOX-x64-2026-10-03/BOX-x64-2026-10-03-vmware.zip) | the same system as a ready-made VMware virtual machine (51 MB) |

Each has a `.sha256` checksum beside it on the Releases page.

### Making the USB stick

You need a **USB stick of 16 GB or more**; everything on it is erased.
Write the `.img.xz` file to it with
[balenaEtcher](https://etcher.balena.io) or
[Raspberry Pi Imager](https://www.raspberrypi.com/software/) (*Use
custom*), on Windows, macOS or Linux. Neither needs the file unpacked
first.

### Booting a PC from it

1. Plug the stick in, and turn **Secure Boot off** in the PC's firmware
   settings.
2. Choose the stick from the firmware's boot menu (often F12, F11, F8 or
   Esc as the PC starts). It must be booted by **UEFI**; old BIOS
   (legacy or CSM) booting is not supported.

The PC starts straight into the RISC OS desktop. Nothing on the PC's own
discs is touched. The stick's 8 GB RISC OS disc keeps whatever you save
on it.

### The hardware it supports

BOX uses Linux's drivers, built into its kernel.

| | Supported |
| --- | --- |
| **Processor** | any 64-bit x86 PC, Intel or AMD, up to 32 threads |
| **Firmware** | UEFI, with Secure Boot off |
| **Graphics** | NVIDIA cards (nouveau), Intel graphics, server boards' remote-console chips (ASPEED, Matrox G200), and any other card through the display the firmware set up, at that one size |
| **Keyboard and mouse** | USB, PS/2, Logitech wireless receivers |
| **Storage** | USB sticks and discs, SATA (AHCI), NVMe |
| **Network** | wired Ethernet: Intel e1000, e1000e, igb, igc; Realtek r8169 |
| **Sound** | Intel HD Audio with the common codecs, HDMI and DisplayPort sound, USB audio |

Not supported in this release: AMD graphics cards (they fall back to the
firmware's display, at one fixed size), Wi-Fi and Bluetooth, legacy BIOS
booting, and Secure Boot.

### In a virtual machine

The same system runs in a virtual machine on a 64-bit Intel or AMD
computer, with UEFI firmware:

- **VMware:** unzip the VMware download. It is set up already, sound
  included.
  - On an **Intel Mac**, open `ROSGD.vmwarevm` in VMware Fusion.
  - On **Windows**, install VMware Workstation (free for personal use)
    and open `ROSGD.vmx` from inside the `ROSGD.vmwarevm` folder.
  - On an **Apple silicon Mac** it cannot run: Fusion there runs only ARM
    virtual machines.
- **QEMU:** with its UEFI firmware (OVMF), the image attached as a USB
  stick. The [release notes](https://github.com/albanread/Aldershot/releases/tag/BOX-x64-2026-10-03)
  give the command line.
- **VirtualBox** is untested, and **Hyper-V** is not supported.

On an Apple silicon Mac the image runs only under full x86 emulation,
which is very slow: use BOX for Apple silicon Macs instead.

### QEMU on Windows

Tested on Windows 11 with QEMU 11.1 from the official Windows installer.

1. Download `qemu-w64-setup-*.exe` from
   [qemu.weilnetz.de/w64](https://qemu.weilnetz.de/w64/), the build linked
   from [qemu.org](https://www.qemu.org/download/#windows), and run it. The
   default folder is `C:\Program Files\qemu`; the commands below assume it.
   The installer includes the OVMF firmware in its `share` folder.
2. Turn on **Windows Hypervisor Platform** (*Turn Windows features on or
   off*) and restart. Without it, use `-accel tcg`, which works but is slow.
3. Unpack the image. Windows has no `xz`, so use 7-Zip, or `xz` from MSYS2:

   ```
   xz -dk BOX-x64-2026-10-03.img.xz
   ```

4. In PowerShell, in the folder holding the image:

   ```powershell
   $q = 'C:\Program Files\qemu'
   Copy-Item "$q\share\edk2-i386-vars.fd" box-vars.fd
   & "$q\qemu-system-x86_64.exe" -machine q35 -accel whpx -cpu qemu64 -smp 4 -m 4G `
     -drive "if=pflash,format=raw,readonly=on,file=$q\share\edk2-x86_64-code.fd" `
     -drive if=pflash,format=raw,file=box-vars.fd `
     -drive if=none,id=stick,format=raw,file=BOX-x64-2026-10-03.img `
     -device qemu-xhci -device usb-storage,drive=stick `
     -device usb-kbd -device usb-tablet -vga std -nic user,model=e1000e `
     -display sdl -audiodev sdl,id=snd -device intel-hda -device hda-output,audiodev=snd
   ```

   The desktop appears in about 30 seconds, at 1280 × 800, with the
   network (10.0.2.15, by DHCP) and sound up.

What each part of the command does:

| Option | What it does |
| --- | --- |
| `-machine q35` | a modern PC with PCIe, which the image's drivers expect |
| `-accel whpx` | runs the guest on the real processor through Windows Hypervisor Platform; `-accel tcg` works without it, but slowly |
| `-cpu qemu64` | a plain 64-bit processor model; not `-cpu host` (see below) |
| `-smp 4 -m 4G` | 4 processors and 4 GB; BOX uses the spare cores for its Worker jobs, so more of each helps |
| `-drive if=pflash,...edk2-x86_64-code.fd` | the UEFI firmware, read-only |
| `-drive if=pflash,...box-vars.fd` | the firmware's settings, a private copy of the template so the original stays clean |
| `-drive if=none,id=stick,...` with `-device qemu-xhci -device usb-storage,drive=stick` | the image, attached as a USB stick, which is how BOX expects to boot |
| `-device usb-kbd -device usb-tablet` | keyboard and mouse; the tablet gives an absolute pointer, so the mouse moves in and out of the window freely |
| `-vga std` | the standard virtual display, which BOX drives at 1280 × 800 |
| `-nic user,model=e1000e` | wired network on QEMU's built-in NAT, using the Intel e1000e driver BOX has; the guest gets 10.0.2.15 by DHCP |
| `-display sdl` | the window, drawn by SDL |
| `-audiodev sdl,id=snd` | sends sound to the Windows default output through SDL |
| `-device intel-hda -device hda-output,audiodev=snd` | an Intel HD Audio card with a speaker output, which BOX has a driver for |

Three things differ from the Linux and Mac command line:

- Use **`-cpu qemu64`**, not `-cpu host`. With `-accel whpx`, OVMF stops
  with a general protection fault before the image starts when `-cpu host`
  is given.
- Use **`-display sdl`**. With the default GTK window on Windows the screen stays
  black and the system never starts.
- For sound use **`-audiodev sdl`** and **`hda-output`**. The `dsound` backend
  failed to start on the test PC (it also opens a microphone), and the
  `wav` backend froze QEMU. Leave out the three sound options for a silent
  machine.

Click inside the window to give it the mouse, and press Ctrl+Alt+G to
release it. The disc image is written to as you work, so keep a copy of
the unpacked `.img` if you want to start again from a clean disc.

---

## BOX on an Apple silicon Mac

The same RISC OS, compiled for Apple silicon instead of x64, on a small
Linux kernel for ARM. QEMU runs it with the Mac's own hypervisor, so it
runs at the speed of the Mac's processor: nothing is emulated.

| Download | What it is |
| --- | --- |
| [BOX-arm64-mac-2026-10-03.zip](https://github.com/albanread/Aldershot/releases/download/BOX-arm64-mac-2026-10-03/BOX-arm64-mac-2026-10-03.zip) | BOX for Apple silicon Macs (73 MB), with a `.sha256` checksum beside it on the [Releases page](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026-10-03) |

It needs an **Apple silicon Mac** (M1 or later) and **QEMU**, which is not
in the download.

### Installing QEMU

1. If you do not have Homebrew, install it from [brew.sh](https://brew.sh):
   paste the command shown on that page into Terminal and follow what it
   says.
2. In Terminal, run:

   ```
   brew install qemu
   ```

   To check it worked: `qemu-system-aarch64 --version`

### Starting it

1. Unzip the download, and keep the folder together: `Start BOX.command`,
   the `system` folder and the `RISCOS` folder stay side by side.
2. Double-click **Start BOX.command**. The first time, macOS may refuse to
   open it because it was downloaded: right-click (or Control-click) it,
   choose **Open**, then **Open** again.
3. A Terminal window opens, then the RISC OS window, and the desktop is
   up in a few seconds.

To stop it, use the desktop's Shutdown, or close the RISC OS window.

### What you get

- **The desktop at your screen's scale:** 1280 × 800, doubled on a Retina
  or HiDPI screen, so the window is sharp and the right size.
- **Your RISC OS disc is a folder:** the `RISCOS` folder is the *Host*
  disc on the icon bar. What you save in RISC OS is kept there, and what
  you put there from the Mac appears in RISC OS.
- **Network** (NetSurf browses the web), and **sound** through the Mac.
- **Settings** such as more memory or processors: see the `ReadMe.txt` in
  the download, or the release notes.

---

## Licence

BOX is **different work from the emulator, under a different licence**,
because it is different work in kind: the RISC OS system is translated to
C, not published as the original sources.

- BOX itself is released under the **Apache 2.0 / MIT** licences.
- It respects the licences of the original authors of the software that
  was translated to C — [RISC OS Open](https://www.riscosopen.org)'s work
  above all. Where a translated component carries an upstream licence
  that asks for more, that component keeps its upstream terms.
- Source code and documentation will follow on; they are still being
  written right now.

---

## Standing on

**[RISC OS Open Ltd](https://www.riscosopen.org)** make RISC OS 5 and
publish it as open source. The system BOX translates is theirs.

The [developer walkthroughs](../walkthroughs/README.md) tell the story of
this project from its beginnings — the same story that led, step by step,
from an emulated Pi to the native BOX.
