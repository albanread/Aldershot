# BOX on a Low-Cost Mini PC

*16 October 2026*

## Introduction

Most of BOX's development has been done on expensive machines: an Apple
silicon Mac Studio, a Mac Pro, and a desktop PC with an Intel i7-12700 and
an NVIDIA T1000 card. None of those is what a person would buy to run
RISC OS on.

In October 2026 we put BOX on a mini PC built round an Intel N97. It is the
cheapest kind of x86 machine sold. It turns out to be a good one. This
article covers:

- the machine, and what one costs;
- how BOX was installed on it;
- the desktop, and what the compositing Window Manager costs it;
- the browser, WebGL and video;
- every RISC OS application we have run on it so far;
- where it is slow, because it is not a fast computer;
- what is not right yet.

The composited desktop these figures describe is the subject of
[The Compositing Wimp](8-Compositing.md).

---

## 1 The machine

The N97 is one of Intel's Alder Lake-N parts, announced at the start of
2023. It has four cores and no hyperthreading, so four threads, a base
clock of 2.0 GHz and a maximum of 3.6 GHz on one core (2.9 GHz on all
four), 6 MB of cache, and an integrated Intel UHD graphics part with 24
execution units running up to 1.2 GHz. Intel's figure for its power is
12 W, and its list price to manufacturers is $128.

The machine we have reports itself as follows:

| | |
| --- | --- |
| Processor | Intel(R) N97, 4 cores, 6 MB cache, 2.6 GHz at the time of reading |
| Memory | 16 GB (15.4 GiB usable), one channel |
| Graphics | Intel UHD Graphics (ADL-N), PCI 8086:46D1, Mesa's `i915` and `iris` drivers |
| Display | HDMI to an LG panel, 3840 x 2160 at 60 Hz |
| Discs | a 953 GiB SK hynix SC311 SATA SSD, and a 476 GiB disc with the Windows 11 it came with |
| Network | Realtek gigabit Ethernet, 1000 Mbit/s |
| Temperature | 46 degrees at the desktop, with the browser running |

Three things about the physical machine are worth saying, because they
are not what a person expects of a computer that does this much work.

It is tiny. It is of the order of 90 mm square and 40 mm deep, and it
sits behind the monitor on the VESA bracket that comes in the box. It
weighs less than the keyboard.

Its power supply is a toy. It is a small plug-top brick of the kind
that comes with a tablet, on a thin lead with a barrel plug. There is
nothing in the machine that needs more.

It has an active fan. It is small and it is audible in a quiet room,
though it is not loud, and at 46 degrees it is not working hard. A
fanless case at this power is possible and some makers sell one; ours
is not fanless.

### What one costs

Prices for these machines move about and the brands come and go, so the
figures below are what the market looked like in October 2026 and should
be checked before buying. They are complete machines: case, power supply,
memory, disc, and a Windows licence that you will not be using.

| Configuration | Rough price |
| --- | --- |
| Barebones kit, no memory and no disc | about 195 pounds |
| 8 GB memory, 256 GB SSD | about 180 to 260 pounds |
| 16 GB memory, 512 GB SSD | about 200 to 300 pounds, or $220 to $330 |
| The same from a brand name (ASUS NUC 14 Essential or Elite) | 340 to 400 pounds |

The useful point is the second and third rows. For the price of a decent
monitor you get a machine that runs the whole of BOX, with the desktop
composited on the graphics part and a WebKit browser playing video, and
has memory and disc left over.

Eight gigabytes is enough. BOX itself asks for very little: with the
desktop up, Ovation Pro loaded with a document, the browser running a
WebGL demonstration and four filer windows open, the machine reported
13.7 GB of its 16 GB free. Sixteen gigabytes is worth the small extra if
the browser is going to be used hard, because WebKit is the one thing
here with a real appetite.

---

## 2 Installing BOX

BOX boots a Linux kernel and then runs as `/init`; there is no Linux
userland to install. See [Is BOX Linux?](5-Linux.md) for what that means.

We first booted it from the same USB stick that boots the T1000 PC. One
stick for both machines needed two changes, made on 16 October:

- the Alder Lake-N display firmware (`adlp_dmc.bin`) had to go into the
  image, along with the Alder Lake-S GuC and HuC firmware that this part
  uses;
- the install tools had to stop assuming drive letters. They now find the
  stick's EFI partition by the GPT partition named "RISC OS disc", because
  on a machine with an internal NVMe disc the stick is `sda` and not
  `sdb`.

Since then the machine has been given over to BOX and starts from its own
SATA SSD. `tools/pc-disc-setup.sh` laid the disc out: a 1 GiB EFI
partition, a 128 GiB RISC OS disc, a second one for a spare, two 16 GiB
WPE roots (A and B, so an update can be rolled back by swapping the
labels), and the rest as a `boxdata` partition. From kernel #99 BOX picks
the RISC OS disc that is on the disc it was started from, which is what
systemd-boot's `LoaderDevicePartUUID` tells it. The stick still boots the
machine and is the way back if a disc install goes wrong.

The Windows disc is untouched. We have not written to it and do not
intend to.

The SSD reads a gigabyte in 1.9 seconds, which is about what 6 Gbit/s
SATA gives. It is not an NVMe disc and it does not need to be.

### Restarting it

This machine would not restart. The Linux kernel's device shutdown hung
on it, every time, and the machine sat there with the light on until
somebody pressed the button. Netconsole with `initcall_debug` showed the
last device the kernel reached was the Realtek Ethernet, but every reset
type - ACPI, EFI, PCI, keyboard controller - hung in the same place, and
disarming Wake-on-LAN made no difference.

SysRq worked. So since kernel #102 every restart path in BOX - the
`ros reboot` command, `tools/pc-install.sh`, and RISC OS's own
`ros_restart` and `ros_poweroff` - synchronises the discs, issues SysRq
`u` to remount read-only, waits, and then issues SysRq `b` or `o`. The
machine now restarts by itself. The driver that hangs has not been
identified, and we would still like to know which it is.

---

## 3 The desktop

![The BOX desktop on the mini PC: Ovation Pro, the browser running a WebGL
demonstration, three filer windows and the icon bar](images/mini-pc-desktop.png)

That is the panel, read back from the graphics part by the presenter and
copied off the machine over the network. It is the whole 3840 x 2160
frame, shown here at half size.

Everything in it is composited. BOX's Window Manager on this machine does
not draw into one screen memory and show it: each window has a picture of
its own, in memory that the presenter maps and uploads to the graphics
part, and the presenter puts the desktop together.
[The Compositing Wimp](8-Compositing.md) describes the arrangement in
full.

What it costs on this machine, taken from the presenter's own minute by
minute figures while the screen shown above was live:

| | |
| --- | --- |
| Frames composited | 3600 in the last minute, that is 60 a second, held |
| Surfaces | 15 (windows, their toolbars, the icon bar, the pointer, a page) |
| Processor time scanning the surfaces | 20 ms a minute |
| Processor time drawing | 1322 ms a minute |
| Everything else | 126 ms a minute |
| Frames where the Wimp's block was missed | 0 |

The drawing figure is the one that matters: 1322 ms of processor time to
produce 3600 frames is 2.2 per cent of one of the four cores. The 24
execution units are doing the work, and they are not troubled by it. The
window shadows and the rounded corners in the picture are drawn by the
presenter as part of the same pass.

The desktop in the picture is a 1600 x 1200 screen shown on a 3840 x 2160
panel, which is why there is black down each side: a 4:3 desktop on a 16:9
panel, scaled to the full height. Taking the panel's own shape instead is
a matter of choosing the mode, but on 24 execution units it is worth
deciding deliberately rather than by accident, because the backdrop scene
is drawn at whatever the screen is.

---

## 4 The browser

BOX's `!Browser` is WPE WebKit running as a Linux program, with its window
delivered to the Wimp as a surface. The arrangement is described in
[The Compositing Wimp](8-Compositing.md). On this machine it uses the same Mesa `iris`
driver that the presenter uses, and VA-API's `iHD` driver for video.

**WebGL is accelerated and fast.** The picture above has
`webglsamples.org/aquarium` open, and the demonstration's own counter
reads **60 fps** on a 1024 x 1024 canvas with the fish swimming. That is
WebGL compiled by Mesa and run on the integrated graphics, inside a
window that the Window Manager is compositing at the same time, on a
twelve-watt processor.

**Video plays, with sound.** YouTube plays in the browser and the sound
comes out of the monitor over HDMI. Getting there took a few days and
three separate problems:

- the box picks the sound output by reading the jack control, so it finds
  the monitor's HDMI output (card 1, device 3) rather than the first card;
- ALSA's default mixing device runs at 48 kHz and refused the loopback
  when BOX captured at 44.1 kHz, which stopped video playing at all on
  kernel #93. Kernel #94 captures at 48 kHz;
- ALSA's `dmix` wanted an `audio` group in `/etc/group`, which the WPE
  root did not have. `/disc/wperun` now binds one over the root's file.

The path a browser sound takes is: WebKit plays to an ALSA loopback card,
BOX captures from the loopback and mixes it with RISC OS's own sound at
48 kHz, and the mixture goes out of HDMI. While the desktop above was
running, the ring held 2688 frames of a 14400 maximum, with no capture
overruns and no card underruns.

---

## 5 What has been run on it

Everything in this table has been run on this machine and watched
working, not merely built for it.

| Software | How it runs | State |
| --- | --- | --- |
| The desktop, filer, icon bar | BOX's native Wimp, composited | Works; 60 fps, 2 per cent of a core |
| Vector icons and painted themes | The presenter | Work |
| Window shadows, rounded corners | The presenter | Work |
| **Ovation Pro** | The ARM container (dynarmic) | Works; documents load, fonts and rulers correct |
| **!Browser** (WPE WebKit) | A Linux program with a Wimp surface | Works; WebGL at 60 fps, YouTube with sound |
| **GraphTask**, the graphical task window | Native | Works |
| **!Mandel** | Native, with Worker threads | Works from kernel #100 |
| **!TBTest** (Toolbox) | Native | Works; it is what the nested-window fix was for |
| Sound, RISC OS and Linux together | The mixer at 48 kHz | Works, over HDMI |
| Ethernet, DHCP, the resolver, TLS | BOX's network modules over Linux | Work |
| SMB sharing of the RISC OS disc | HostFS and the SMB server | Works |
| The control shell on port 2222 | Linux busybox, started before RISC OS | Works; see section 2 |

The rest of BOX's software is on this machine's disc and is the same
build as on the other machines, where it is tested: NetSurf, StrongED,
PipeDream, !PDF, !AWViewer, !Clock, !Maestro, !Breakout, !Doodle,
!CloseUp and the others in the picture above. We have no reason to think
any of them behaves differently here, since nothing below the application
differs, but this article only claims what we have watched run on this
machine.

Two features of this machine's software are new since the T1000 PC and
were proved here: nested child windows now follow their parent by their
alignment flags, which Toolbox text gadgets need; and a BASIC text program
started from the desktop runs in GraphTask with a display window of its
own, which is what `!Mandel` wanted.

---

## 6 It is not a compute machine

Nothing above should be read as a claim that this is a fast computer. It
is not. The four cores are Gracemont efficiency cores, there is no
hyperthreading, and one memory channel feeds all four. The N97 exists to
be cheap and to sip power, and it does both.

What it is good at is the work the desktop actually asks for: moving
pixels about, which the graphics part does; and running one application's
single thread, which it does well enough that Ovation Pro and the filer
feel immediate. RISC OS is a co-operative, single-threaded desktop, so
most of the time three of the four cores have nothing to do anyway.

Give it something that genuinely wants processors and the limit shows at
once. `!Mandel` divides the Mandelbrot set across Worker threads, and
it will take every core it is given. Here it gets four slow ones. It runs, and it is a good deal
quicker than one core would be, but it is the place where you would
rather have the i7-12700, or an Apple silicon Mac, or anything with more
cores and faster ones. The same goes for compiling: `*CC` on this machine
is patient work.

So: a fine desktop machine, and a poor number-cruncher. That is a
reasonable bargain at this price, and it is worth knowing which side of
it your work falls on before buying one.

---

## 7 What is not right yet

- **Wi-Fi is not proven.** The kernel has the drivers from 16 October
  (`rtw88` for the 8821CE, `cfg80211`, `mac80211`, and Bluetooth over
  `btusb`), but `wlan0` on this machine is down and we have not had it on
  a network. Ethernet is what we use.
- **No eMMC.** The kernel has no MMC support, so a machine whose only
  storage is eMMC will not do. Ours has SATA and that is what we use.
- **The desktop does not fill the panel,** as section 3 explains.
- **`INTEL_IDLE` is not in the kernel**, so the processor does not use the
  deeper idle states. The performance governor is in use. Neither has
  caused trouble at 46 degrees, but both are worth revisiting.
- **A 4K panel on 24 execution units** is the one place where this
  machine could be made to struggle, and the backdrop scene is the thing
  that would do it.
- **The hanging driver** in the restart path, from section 2, is still
  unidentified.

---

## 8 In short

A twelve-watt processor that costs the maker $128, in a case the size of a
sandwich, with a power supply that looks like a phone charger, runs the
whole of RISC OS with a composited desktop at 60 frames a second, a
WebKit browser doing accelerated WebGL at the same rate, and video with
sound, and uses about two per cent of one of its four cores to do the
compositing.

It is not a fast computer, and section 6 says where that shows. But a
person who wants a RISC OS machine on a desk, for about 200 pounds, now
has one, and it gives a better RISC OS application experience than the
hardware's price suggests. That is a better position than RISC OS has
been in for some time, and it is not because the software got faster. It
is because the cheapest computers got fast enough.
[Ancient and Modern](6-Ancient.md) says more about why that matters.
