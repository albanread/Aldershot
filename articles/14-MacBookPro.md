# BOX on the 2019 MacBook Pro

*30 October 2026*

**Note: this is work in progress.** We have one of these laptops, and
this article describes that one. It works well and is in daily use, but
a laptop has many more devices than a desktop, several of them are
reached through Apple's T2 chip, and some are not done yet (section 4).
We cannot promise that yours will behave as ours does. Installing BOX
erases the Mac. [BOX on Second-Hand Intel Macs](12-Macs.md) covers preparing the
machine and getting macOS back.

## Introduction

The 16-inch MacBook Pro of late 2019 was the last and largest Intel
MacBook Pro. It has a 3072 x 1920 Retina display, six speakers, a Touch Bar, and two graphics
parts: Intel's, built into the processor, and an AMD Radeon. On eBay in
October 2026 a working one costs about 300 to 350 pounds.

Ours was bought on eBay to run BOX, and has run it from its own SSD
since 26 October 2026, with macOS erased. This article covers:

- the machine we have, and how others differ;
- which parts of the hardware BOX uses so far;
- what software we have run on it;
- what is not done yet.

---

## 1 The machine

| | |
| --- | --- |
| Model | MacBookPro16,1, 16-inch, 2019 |
| Processor | Intel Core i9-9980HK, 8 cores, 16 threads, 2.4 GHz |
| Memory | 32 GB |
| Graphics | Intel UHD Graphics 630, and AMD Radeon Pro 5300M with 4 GB |
| Display | built-in Retina panel, 3072 x 1920 |
| SSD | Apple 1 TB, behind the T2 chip |
| Network | Wi-Fi and Bluetooth (Broadcom BCM4364); no Ethernet socket |
| Ports | four Thunderbolt 3 / USB-C |

The model was also sold with a six-core i7 and with the Radeon Pro 5500M
or 5600M. We have tried only the i9 with the 5300M. An i7 should behave
the same, a little slower. The 5500M is a close relative of the 5300M and
we expect it to work; the 5600M is a different kind of part and is the
one we would be least sure of. The Intel graphics are the same in all of
them, so every version has at least that to fall back on.

---

## 2 What works

| Hardware | State | Notes |
| --- | --- | --- |
| Display | Works | the full 3072 x 1920, and desktop modes of the same shape, down to 1024 x 640, that fill the panel |
| Intel graphics | Works | the cooler of the two; Mesa's `iris` driver |
| AMD Radeon 5300M | Works | Mesa's `radeonsi` driver |
| Choosing the graphics | Works | the Task Manager's menu has Restart on Intel and Restart on AMD; the choice needs a restart |
| Brightness | Works | fn+F1 and F2, and it follows the room's light; the screen goes out after 10 minutes untouched |
| Keyboard | Works | |
| Keyboard light | Works | lit when a key is pressed in a dark room, out 30 seconds after the last key; fn+F5 and F6 set its level |
| Trackpad | Works | moves the pointer; one finger pressed or tapped is Select, two Menu, three Adjust; two fingers moved scroll |
| Touch Bar | Works | shows Escape and F1 to F12; holding fn shows brightness and volume; it dims after a minute and lights again at a touch |
| Speakers and headphone socket | Works | the plug is followed; the volume keys set BOX's own level, which starts at 30% |
| Battery | Works | a Battery icon on the icon bar, warnings, and DeskMeter's Battery view, which records a day's charge |
| Lid | Partly | shutting it turns the screen and keyboard light off; it does not sleep |
| Internal SSD | Works | |
| Wi-Fi | Works | needs Apple's firmware copied from macOS first |
| Bluetooth | Works | |
| USB-C | Works | USB devices, including a USB-C Ethernet adapter (ours is an ASIX AX88179A) |
| Fans and temperatures | Works | BOX's heat guard drives both fans; the processor is held to 80 degrees |
| Sleep | **Not done** | tried on 26 October and put aside; see section 4 |
| Camera and microphones | **Not done** | the kernel sees them; nothing in BOX uses them yet |
| Pinch, rotate and swipe gestures | **Not done** | RISC OS has no meaning for them yet |
| Thunderbolt devices | **Not tested** | they are let in as they are plugged, but we have tried none |

The sound needs a warning. The T2's sound has no volume control of its
own, so BOX supplies one. A program that plays at RISC OS's full volume
is loud through these six speakers; keep the level well down until you
know what a program does.

---

## 3 What has been run on it

![The 2019 MacBook Pro's display: the RISC OS disc and its Apps
directory in Filer windows, with applications loaded on the icon
bar](images/macbook-applications.png)

![The same display a minute later: a Tiger drawing in !Trace beside a
game of !Tower](images/macbook-trace-tower.jpg)

Those are the display as the presenter composited it, read back from the
graphics part over the network on 30 October 2026 and shown at half size.
The icon bar shows how much was loaded at the time: !Palette, the browser,
StrongED, !Lisp, !Smalltalk, !Folio, !Ledger and others, with the Battery
icon. With all of that running the laptop was using 2.1 GB of its 32 GB.

Everything in this table has been run on our MacBook and watched
working.

| Software | What it is | Notes |
| --- | --- | --- |
| The desktop, Filer, icon bar, Task Manager | BOX's own Window Manager, composited | on either graphics part |
| !Trace | our vector drawing application | the Tiger sample, opened from the RISC OS disc |
| !Tower | our tower-building game | a saved game |
| !Antiquity | a 3D strategy game, still in development, playing back a recorded game | Smooth and Rich settings, four minutes at a time, to test the heat limit |
| Battery and DeskMeter | the icon bar's battery, and DeskMeter's Battery and Thermals views | |
| Task windows | the command line in a window | F12 on the Touch Bar opens one |
| !Network | Wi-Fi and Bluetooth settings | |

The rest of BOX's applications are on this MacBook's disc and are the
same build as on our other machines. Several of them are in the pictures
above. We expect them to work, but this article only claims what we have
watched run here.

---

## 4 What is not right yet

- **No sleep.** Shutting the lid turns the screen off, but the machine
  keeps running and the battery keeps draining. When we tried real sleep
  on 26 October the laptop did not come back and needed its power button,
  so we put it aside rather than risk it. Shut the machine down from the
  Task Manager when you put it away.
- **Changing graphics needs a restart.** That is how the hardware is
  wired: the display is given to one part or the other as the machine
  starts.
- **The camera and the microphones** are not used.
- **Only one machine tested,** with the i9 and the 5300M (section 1).
- **The firmware is slow to start** with macOS gone, because it looks for
  a macOS start-up disc first. It does start.

---

## 5 In short

The 2019 MacBook Pro 16 makes a fine BOX laptop. The display, keyboard,
trackpad, Touch Bar, speakers, battery and both graphics parts all work,
though much of it sits behind the T2 chip and took the most work of the
three Macs. Sleep is the one large thing missing.

It has more processor and memory than BOX needs; 16 GB is plenty (see
[BOX on Second-Hand Intel Macs](12-Macs.md), section 3). It is one tested machine
and still work in progress, so try it from the USB stick before you
erase anything.

