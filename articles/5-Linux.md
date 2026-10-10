# Is BOX Linux?

*3 October 2026; revised 10 October 2026*

## Introduction

RISC OS is a 32-bit, single-process, interrupt-driven masterpiece. I
believe no port to 64 bits can be RISC OS, or should be. The best I can
do is create a RISC OS application environment for 64-bit computers,
and that is what BOX is.

People ask whether BOX is Linux. The answer is yes and no.

Yes, because BOX boots a Linux kernel, and Linux does a great deal of
the work. No, because nothing of Linux as a user knows it is there:
no shell, no distribution, no Linux desktop. What you see and use is
the RISC OS application environment: the desktop, the `*` prompt, BBC
BASIC and RISC OS programs, behaving as they always have.

This article describes what Linux provides, the work that saved, how
Linux supports RISC OS, and why there is no Linux userland.

---

## 1. Yes: BOX boots Linux

When BOX starts, the first thing to run is a Linux kernel, version
6.18, unmodified apart from its configuration. The kernel starts one
program, `/init`, and `/init` is RISC OS: the translated and rewritten
modules, the runtime, and the desktop, all in a single executable.

The same kernel source is built for each machine BOX runs on:

| Machine | How it runs |
| --- | --- |
| Apple silicon Mac | an arm64 kernel under Apple's Virtualization framework |
| Intel Mac | an x86-64 kernel under QEMU with HVF |
| PC | an x86-64 kernel booted straight from a USB stick by UEFI, with no boot loader; the ROM is built into the kernel image |

---

## 2. What Linux provides

### Portability

Linux runs on almost everything. Because RISC OS in BOX talks only to
Linux, never to the hardware, moving BOX to a new machine is mostly a
matter of building a kernel with the right options. Going from the
Intel box to Apple silicon needed a new compiler mode (A64X32) and some
runtime machine code. It needed no new RISC OS drivers.

### Drivers

| Area | What Linux supplies |
| --- | --- |
| Display | DRM: virtio-gpu in a virtual machine; the firmware's framebuffer (simpledrm) and VMware's adapter on a PC. RISC OS draws into a DRM buffer mapped into the arena. |
| Keyboard and mouse | evdev, over virtio input, USB HID, and PS/2 |
| Storage | virtio block devices, SATA (AHCI), NVMe, USB mass storage; the ext4 filing system |
| Sound | ALSA: virtio sound, Intel HD Audio |
| Network | virtio-net; Intel e1000, e1000e, igb and igc; Realtek r8169; VMware vmxnet3 |
| Other | the real-time clock, a random number source, USB host controllers (xHCI, EHCI, OHCI, UHCI) |
| Sharing with the host | 9P and virtiofs, which HostFS uses for the Mac's own files |

### Networking

* TCP/IP, IPv4 and IPv6, with Linux's own stack
* DHCP, done by the kernel at boot
* name lookup, through Linux's resolver

### Network services

* **Windows and Mac file shares:** Linux's SMB2/3 client (cifs), and
  ksmbd, its in-kernel SMB3 server
* **SSH:** OpenSSH's client, key generator and server
* **TLS:** OpenSSL 3, giving TLS 1.3
* **HTTP and HTTPS:** libcurl

### The basics of an operating system

Memory (each RISC OS region is a Linux memory object), threads (each
RISC OS task is one), timers, pseudo-terminals, and the handling of
faults.

---

## 3. The porting work it saved

On a Raspberry Pi, RISC OS drives the hardware itself, and each new
machine has meant new RISC OS drivers, written and maintained by a
small community. BOX carries none of these:

* **The HAL**, the per-board hardware layer
* **BCMVideo**, which drives the Pi's GPU through its mailbox
* **The USB stack**, including its keyboard and mouse drivers
* **The Ethernet drivers** (EtherGENET, EtherUSB), with **DCI4**, the
  driver interface, and **MbufManager**, which the drivers and the
  stack share buffers through
* **The 4.4BSD TCP/IP stack** inside the Internet module
* **Storage drivers** for SD cards and USB discs
* **Econet** and the other old network drivers

The RISC OS modules that clients call are kept, so programs see the
same SWIs. Underneath, each is now a short module that calls Linux:

| RISC OS module | Now works through |
| --- | --- |
| Internet | Linux sockets |
| Resolver | Linux's resolver |
| DHCP | the kernel's DHCP lease |
| LanManFS | Linux's SMB2/3 client, in place of SMB1, which modern servers refuse |
| ShareFS (Acorn Access) | ksmbd, an SMB3 server Macs and PCs can use |
| AcornSSL | OpenSSL 3, in place of mbedTLS, which stops at TLS 1.2 |
| URL_Fetcher, AcornHTTP | libcurl |
| GraphicsV driver | DRMVideo, over Linux's DRM |
| Keyboard and pointer | Input, over evdev |
| SharedSound | ALSA |
| HostFS | Linux's own files |

Most of these are small. Six of them (AcornSSL, URL_Fetcher and
AcornHTTP, LanManFS, the SMB server and the SSH server) were built on a
single day, 26 September 2026. Writing a TCP/IP stack,
TLS 1.3, an SMB3 client and server, and drivers for every network,
graphics, storage and USB controller a PC might have, would have been
years, and the work would never have ended.

RISC OS also gains things it never had: an SSH client and server, TLS
1.3, SMB3 file sharing with Macs and Windows, and IPv6.

---

## 4. How Linux supports RISC OS

Linux is to BOX what the HAL and the hardware drivers are to RISC OS on
a Pi. RISC OS never sees the hardware, and Linux never sees RISC OS's
own work.

* **Everything is called from RISC OS.** When RISC OS wants to read a
  disc, draw on the screen or send a packet, the module concerned calls
  Linux at that moment, on the RISC OS task's own thread. Linux runs no
  RISC OS code of its own accord.
* **One program.** RISC OS is the first and only process Linux starts.
  There is no second process for the operating system and no messages
  passing between them.
* **Memory.** The low 4 GB of `/init`'s address space is the RISC OS
  address map. Each region (the application slot, the RMA, the screen)
  is a Linux memory object, mapped at its RISC OS address.
* **Faults.** If a RISC OS program faults, Linux passes the fault to
  `/init`, which turns it into the RISC OS error the program expects. The
  box carries on.
* **Linux programs, when asked.** A few things RISC OS uses are Linux
  programs: `ssh`, for example. RISC OS runs them as child processes on
  a pseudo-terminal, shows their output in its own text window and
  passes them its keyboard. If one crashes, only that child process
  ends.
* **Daemons, when asked.** `*SSHD` and `*Share` start OpenSSH's server
  and ksmbd's user-space half, and `/init` looks after them.

---

## 5. No: there is no Linux userland

Linux as most people know it is not the kernel but everything above it:
a shell, a shared C library, system services, a package manager and a
desktop. BOX has none of these. Its programs carry their own C library,
built into each one (see "Alpine and musl", below).

The whole of BOX's root filing system is:

* `/init`, which is RISC OS;
* `/dev`, `/proc` and `/sys`, which the kernel fills in;
* a handful of static programs that RISC OS commands run: clang, lld,
  rosbas and roscc for `*CC` and `*RosBas`; `ssh`, `ssh-keygen` and
  `sshd`; and ksmbd's tools;
* the headers and libraries those compilers need.

There is no shell to fall back to, no `/bin`, no `/etc` beyond the one
or two files RISC OS writes, no init system, no login, and no dynamic
loader. Nothing runs unless RISC OS starts it. Even an SSH login to BOX
gives you a RISC OS `*` prompt, not a Linux one.

`*CC --linux` can build a Linux program, and `*RunBox` will run it. That
is a RISC OS command running a guest; it does not give you a Linux
system.

### Alpine and musl

Two names turn up in BOX's build. Neither makes BOX a Linux
distribution.

**Alpine Linux** is a small Linux distribution, commonly used for
containers and embedded systems, where a full desktop distribution would
be far too large. BOX uses it only while it is being built: the kernel
is compiled inside an Alpine virtual machine, because a Mac's
case-insensitive disc cannot hold the kernel's source tree. None of
Alpine ends up in BOX.

**musl** is a small C library, a lightweight alternative to glibc, the
C library most Linux distributions use. It is designed to be linked
statically: a program carries the parts of the library it uses, and
needs nothing else on the disc. That suits BOX, whose root filing system
has no shared libraries and no dynamic loader. `/init` is linked with
musl, and so are the static programs listed above (clang, OpenSSH and
ksmbd's tools) and the libraries AcornSSL and the URL fetcher use
(OpenSSL and libcurl). `*CC --linux` builds Linux programs against musl
too.

---

## 6. Is that a disappointment?

In some ways, yes. Much of the fun of RISC OS has always been that it
is not a mini-computer operating system. It is small, it is quick, and
it is close to the machine: one person could understand all of it, and
a BASIC program could poke the hardware. Putting a Unix kernel
underneath, however well hidden, gives some of that up, and it would be
wrong to pretend otherwise.

But it is worth being honest about how RISC OS is used today. A great
many people already run it on top of another operating system: RPCEmu
or Arculator on Windows, Linux or macOS. In
those cases RISC OS sits in a window, with Windows or macOS (and all
their drivers) underneath it. BOX is no worse than that, and in one way
better: on a PC, BOX boots straight from a USB stick, with no other
operating system in sight. The machine starts, and RISC OS is what you
get.

---

## 7. The vision

BOX is not Linux with a RISC OS window on it, and it is not an emulator
running in a Linux desktop. It is a RISC OS application environment,
behaving as RISC OS has always behaved:

* the machine starts straight into the RISC OS desktop, through the
  RISC OS boot sequence and `!Boot`;
* the screen belongs to the Window Manager;
* F12 gives the `*` command line, and `*` commands, Obey files and
  BBC BASIC work as they always have;
* files are RISC OS files, with file types, seen through the Filer;
* programs are RISC OS programs, using RISC OS's SWIs, and they run as
  they did.

Linux is there so that RISC OS can run on modern hardware, reach modern
networks and talk to modern machines, without the RISC OS community
having to write and maintain all of that itself. It does that job
underneath and out of sight.

So: BOX boots Linux, and depends on it. But BOX is not Linux. It is a
RISC OS application environment for 64-bit computers.
