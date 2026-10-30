# BOX on Second-Hand Intel Macs

*30 October 2026*

**Note: this is work in progress.** Everything below describes three
machines we bought on eBay and gave over to BOX in October 2026. They
work well, and we use them every day, but we have tested one machine of
each kind, and a different build of the same model may behave
differently. Nothing here is a guarantee that BOX will work on your
Mac. Read section 4 before you erase anything.

**Note: this is not RISC OS.** BOX is a RISC OS window manager and
application environment running on a Linux kernel. It runs translated
RISC OS applications, and some emulated ones. [Is BOX Linux?](5-Linux.md) and [Emulation in BOX](2-Emulation.md) say what that means.

## Introduction

Apple stopped selling Intel Macs in 2023, and the later ones are now
cheap on eBay. They are well made, they are quiet, most of them have
good displays and fast SSDs, and their Intel processors and graphics are
supported by Linux. That makes them good machines for BOX.

We have put BOX on three of them. Each has its own article:

| Machine | Article |
| --- | --- |
| Mac mini, 2018 (Macmini8,1) | [BOX on the 2018 Mac mini](13-MacMini.md) |
| MacBook Pro 16-inch, 2019 (MacBookPro16,1) | [BOX on the 2019 MacBook Pro](14-MacBookPro.md) |
| Mac Pro, late 2013 (MacPro6,1) | [BOX on the 2013 Mac Pro](15-MacPro.md) |

This article covers what the three have in common:

- what BOX does to the machine;
- what each of them costs on eBay;
- how much memory is worth having;
- how to prepare a Mac for BOX;
- what to do if you want macOS back.

The other articles cover what works on each machine, what has been
tested and what has not.

---

## 1 BOX takes over the whole machine

On these Macs BOX is not an application that runs under macOS. It is the
operating system. The Mac starts a Linux kernel, the kernel starts BOX,
and BOX owns the screen, the keyboard, the discs and the network.
There is no macOS underneath it and no Linux desktop beside it.

When BOX is installed on the Mac's internal SSD, the SSD is erased and
laid out afresh for BOX:

| Partition | What it holds |
| --- | --- |
| EFI system partition, 1 GB | the boot loader and the kernels |
| RISC OS disc | your files and applications, seen as `HostFS::Disc` |
| A spare RISC OS disc | room for a second disc, or a copy |
| Two WPE roots, A and B | the browser's Linux libraries, two copies so that an update can be undone |
| BOX data | everything else |

macOS, its recovery partition on the SSD and every file on the machine
are gone after this. That is why we only do it on machines bought for
the purpose, and why section 4 asks you to try BOX from a USB stick
first.

---

## 2 What they cost

These are eBay UK "Buy It Now" prices for working machines, read on 30
October 2026. Prices move about, and a machine's condition matters as
much as its specification, so check before buying.

| Machine | Typical configuration | Rough price |
| --- | --- | --- |
| Mac mini 2018 | quad-core i3, 8 GB, 128 or 256 GB | 160 to 190 pounds |
| | six-core i5, 8 GB, 256 or 512 GB | 200 to 260 pounds |
| | six-core i7, 16 or 32 GB, 512 GB (as ours, but ours has 10 Gb Ethernet) | 340 to 370 pounds |
| MacBook Pro 16-inch 2019 | six-core i7, 16 GB, 512 GB | 300 to 340 pounds |
| | eight-core i9, 16 or 32 GB, 1 TB (as ours) | 300 to 350 pounds |
| Mac Pro late 2013 | quad-core Xeon, 12 to 16 GB, 256 GB, FirePro D300 (as ours) | 145 to 200 pounds |
| | six- to twelve-core Xeon, 32 to 64 GB, D500 or D700 | 250 to 400 pounds |

A good deal of what is listed under these names is sold for parts: cracked
screens, liquid damage, missing SSDs. The cheapest MacBook Pro 16 listings
are nearly all of that kind, at 150 to 200 pounds. Read the description.

Three things are worth asking the seller before buying:

- **Is the Mac signed out of iCloud, with Find My turned off?** A Mac
  still locked to its last owner's Apple account will not let you change
  its start-up security.
- **Is there a firmware password?** With one set, the Mac will not let
  you choose the USB stick at start-up.
- **Does it start macOS, and can you log in as an administrator?** On the
  2018 and 2019 machines you need an administrator's name and password to
  allow the Mac to start from a USB stick (section 4).

A machine sold with macOS freshly installed and no account set up is the
easiest: you make the administrator account yourself on first start.

---

## 3 How much memory

BOX itself uses very little memory. Measured on our machines on 30
October 2026, the figures Linux gives as "used":

| Machine | What was running | Memory used |
| --- | --- | --- |
| Mac mini | the desktop, Micropolis with a new city | 332 MB |
| Mac mini | the same, with a BBC BASIC Mandelbrot in a graphics window | 434 MB |
| MacBook Pro | the desktop with about a dozen applications loaded, among them !Trace with a drawing, !Tower with a game, the browser, StrongED, !Lisp and !Smalltalk | 2.1 GB |

So:

- **8 GB is enough** for the desktop and RISC OS applications, with
  plenty to spare.
- **16 GB** is worth having if you use the browser hard. The browser is
  WebKit, a Linux program, and it is the one thing in BOX with a real
  appetite for memory. With 16 GB several browser windows can be open
  at once without any concern.
- **More than 16 GB** is not used by anything in our testing. Our
  MacBook Pro has 32 GB and most of it is always free. Do not pay extra
  for it on BOX's account.

---

## 4 Preparing your Mac

This is the order we followed on our machines. The steps marked "T2" are
for the 2018 Mac mini and the 2019 MacBook Pro, which have Apple's T2
security chip. The 2013 Mac Pro has no T2 and skips them.

1. **Update macOS as far as it will go,** while you still have it. The
   Mac's firmware is updated along with macOS, and BOX relies on the
   newest firmware. The Mac mini 2018 stops at macOS 15 Sequoia; the
   MacBook Pro 16 goes further.
2. **Copy the Wi-Fi firmware (T2).** The Wi-Fi chips in the mini and the
   MacBook need firmware files that belong to Apple and come with macOS.
   We are not allowed to give them away, so they must be copied from the
   Mac's own macOS, from `/usr/share/firmware/wifi`, before macOS is
   erased. Without them BOX works on Ethernet only. The 2013 Mac Pro
   does not need this.
3. **Allow the Mac to start from a USB stick (T2).** Restart and hold
   Command-R as the Mac starts, to reach macOS Recovery. Choose
   Utilities, then Startup Security Utility, give the administrator's
   password, and set:
   - Secure Boot: **No Security**;
   - External Boot: **Allow booting from external or removable media**.
4. **Start BOX from the USB stick.** Plug the stick in, restart, and
   hold the Option key at the chime (or as soon as the screen lights).
   The Mac shows its start-up discs; choose the one labelled "EFI Boot".
   BOX starts on the stick and leaves the internal SSD alone.
5. **Try it.** Check the screen, the keyboard, the pointer, the network
   and the sound before going further. If anything you need does not
   work, stop here: switch off, take the stick out, and the Mac is as it
   was.
6. **Install to the internal SSD.** This is the step that erases macOS
   and every file on the Mac (section 1). It cannot be undone except by
   reinstalling macOS (section 5). After it the Mac starts BOX by itself,
   with no stick and no key held down.

On a Mac with no Ethernet socket (the MacBook Pro) a USB-C Ethernet
adapter is needed until Wi-Fi is set up. We use one built on the ASIX
AX88179A chip.

### Where the installer stands

So far we have installed our three machines with our own tools, from a
running BOX, over the network. A single USB stick that starts all three
Macs, with a desktop installer (!Install) that recognises the machine and
asks you to type ERASE before it touches the SSD, has been built and tried
in a virtual machine. It has not yet been tried on the real Macs, and it
has not yet been published. Until it is, the steps above describe the
process rather than a finished product.

---

## 5 Getting macOS back

Nothing BOX does is permanent to the hardware. It never touches the T2
chip or the firmware.

- **macOS by Internet Recovery.** Hold Option-Command-R as the Mac starts.
  It downloads macOS Recovery from Apple, which can erase the SSD and
  install macOS again. This needs a network connection (Ethernet is the
  simplest) and some patience.
- **Apple Configurator (T2).** If a T2 Mac will not start at all, another
  Mac running Apple Configurator can restore it over a USB-C cable. We
  have not needed to do this.
- **Start-up security.** Once macOS is back, you may wish to return
  Startup Security Utility to Full Security.

---

## 6 Heat

All three Macs are small cases with a lot of processor in them, and
macOS normally manages their fans. Under BOX that job falls to BOX's
heat guard, which reads the Mac's temperature sensors and drives the fans.
On the Mac mini and the MacBook Pro it also holds the processor to 80
degrees by limiting its turbo speed, and if the guard stops, BOX restarts
the machine rather than leave the fans at whatever speed they were. The
Mac Pro still has the earlier guard, which slows the processor at 85
degrees; [BOX on the 2013 Mac Pro](15-MacPro.md) says more.

In ordinary desktop use none of the three gets warm. Running every core
flat out for minutes at a time is where the 80-degree limit takes over,
and the machine then runs a little below its top speed. We decided on
the limit deliberately: we would rather have a slightly slower Mac than
a hot one.

---

## 7 In short

A second-hand Intel Mac from eBay, for between 150 and 350 pounds, makes
a good BOX machine: a quiet, well-built computer with a composited RISC OS
desktop, the browser, and the RISC OS applications BOX runs. Eight
gigabytes of memory is enough and sixteen is plenty.

It is still work in progress. We have tested one of each model, each
article lists what we have and have not tried, and installing BOX erases
the Mac. Try it from the stick first.

