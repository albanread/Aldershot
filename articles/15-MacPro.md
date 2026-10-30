# BOX on the 2013 Mac Pro

*30 October 2026*

**Note: this is work in progress.** We have one of these machines, and
this article describes that one. It was the first Mac we gave over to
BOX, and it now runs older BOX software than the other two (section 4).
We cannot promise that yours will behave as ours does, especially if it
has different graphics cards (section 1). Installing BOX erases the Mac.
[BOX on Second-Hand Intel Macs](12-Macs.md) covers preparing the machine and getting
macOS back.

## Introduction

The late 2013 Mac Pro is the black cylinder. It has a Xeon processor,
two AMD FirePro graphics cards, and a single fan that draws air up
through the middle. It was expensive when new and is cheap now: on eBay
in October 2026 a quad-core one costs about 145 to 200 pounds.

Ours was bought on eBay to run BOX, and has run it from its own SSD
since 20 October 2026, with macOS erased. There are no screenshots in
this article: the machine was switched off while it was being written.
This article covers:

- the machine we have, and how others differ;
- which parts of the hardware BOX uses so far;
- what software we have run on it;
- what is not done yet.

---

## 1 The machine

| | |
| --- | --- |
| Model | MacPro6,1, late 2013 |
| Processor | Intel Xeon E5-1620 v2, 4 cores, 8 threads, 3.7 GHz (Ivy Bridge) |
| Memory | 16 GB |
| Graphics | two AMD FirePro D300, 2 GB each |
| Display | an LG 4K monitor on the HDMI socket |
| SSD | Apple 256 GB |
| Network | two gigabit Ethernet sockets (Broadcom), Wi-Fi (Broadcom BCM4360) |

This Mac Pro is older than the other two and has no T2 chip, so there is
no start-up security to change: hold Option at the chime and choose the
USB stick.

The machine was sold with four, six, eight and twelve-core Xeons, and with
D300, D500 or D700 graphics. We have tried only the quad-core with the
D300s. More cores make no difference to BOX. The D500 and D700 are a
larger part of the same generation as the D300, and we expect them to
work, but we have not tried one.

---

## 2 What works

| Hardware | State | Notes |
| --- | --- | --- |
| Graphics | Works | the desktop composited by the presenter on one D300, using Mesa's `radeonsi` driver |
| The second graphics card | Sleeps | a second card gains nothing for one monitor, so BOX leaves it switched off |
| 4K display on HDMI | Works, at 30 Hz | the HDMI socket is HDMI 1.4, which cannot do 4K at 60 Hz; we have not tried the Thunderbolt sockets for a display |
| Video in the browser | Works | H.264 is decoded by the card, with the processor almost idle; the browser picks H.264 on YouTube |
| WebGL in the browser | Works | the WebGL aquarium, with 10,000 fish, at the monitor's 30 frames a second |
| Sound through the monitor (HDMI) | Works | |
| Ethernet | Works | |
| Wi-Fi | Works | Broadcom's own driver; no Apple firmware is needed; networks rejoined at start-up |
| USB keyboard and mouse | Works | |
| Internal SSD | Works | |
| Fan and temperatures | Works | BOX's heat guard drives the fan; the cards' clocks are kept low, because these cards overheat in this case |
| Bluetooth | **Not tested** | |
| Thunderbolt devices | **Not tested** | |
| Sleep | **Not done** | |

The cards deserve a word. The D300s run hot in this case, and the Mac
Pro is known for it. BOX keeps their clocks down, in particular while
they decode video, and that has been enough: 450 MHz decodes 1080p H.264
at 77 frames a second, more than any video needs.

---

## 3 What has been run on it

Everything in this table has been run on our Mac Pro and watched
working, as of 20 October 2026.

| Software | What it is | Notes |
| --- | --- | --- |
| The desktop, Filer, icon bar, Task Manager | BOX's own Window Manager, composited | |
| The browser | WebKit, with its windows on the RISC OS desktop | YouTube with sound, and WebGL |
| !Code | BOX's development environment | with its Tcl mode |
| !Workflows and !Draft | Tcl workflows, and Markdown and diagrams drawn by BOX | |
| Mojo | the compiler, on the machine | each of the above was compiled here, in about 27 seconds |
| Wi-Fi from !Boot | joining a network at start-up | |

One thing found on this machine affects the others: its Ivy Bridge
processor is older than the processors our Intel builds expected, and the
Mojo compiler stopped with an illegal instruction until a build for the
older processor was made. That build is now the one used on this machine.

---

## 4 What is not right yet

- **Older software.** This machine runs BOX as it was on 20 October
  2026. The other two Macs have had ten days more work, including the
  heat guard's 80-degree limit and the restart if the guard stops. The
  Mac Pro's heat guard is the earlier one, which slows the processor at
  85 degrees. It needs bringing up to date, and some of what is on the
  other two (the newer way of loading drivers, for example) has not been
  proved here.
- **4K at 30 Hz** on the HDMI socket. A monitor on one of the Thunderbolt
  sockets might do better; we have not tried it.
- **Only one machine tested,** with D300 graphics (section 1).
- **No sleep.**

---

## 5 In short

The 2013 Mac Pro is the cheapest of the three Macs on eBay, and BOX runs
well on it: the composited desktop, the browser with video decoded by the
graphics card, Wi-Fi and Ethernet. It is a big, quiet machine with more
graphics than BOX can use.

It is also the oldest, its cards run hot, and our BOX on it is ten days
behind the other two. It is one tested machine and still work in
progress. For a new BOX machine we would buy the Mac mini first.

