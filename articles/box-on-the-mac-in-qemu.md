# BOX on the Mac, in QEMU

*3 October 2026*

## Introduction

BOX runs on Apple silicon Macs as well as on PCs. The Mac version is
the same RISC OS, translated to C, but compiled for the Mac's own ARM
processor rather than for x64, and run on a small Linux kernel for ARM.

Something has to start that kernel in a virtual machine. For the
public release, that is QEMU, using the Mac's own hypervisor. This
article describes how the pieces fit, what QEMU provides and what it
does not, and the few things that had to be done to make it behave.

---

## 1. Two hosts for one system

BOX on the Mac can be started in two ways. Both run exactly the same
kernel and the same ROM.

| | Apple's Virtualization framework | QEMU |
| --- | --- | --- |
| Launcher | `rosgd-vz`, our own program | Homebrew's `qemu-system-aarch64` |
| Processor | the Mac's, through Apple's hypervisor | the Mac's, through Apple's hypervisor (`-accel hvf`) |
| The share (the Host disc) | virtio-fs | 9P |
| Used for | development, and the test harness | the public release |

Neither emulates a processor. In both, RISC OS's code runs directly on
the Mac's cores; the hypervisor only keeps the virtual machine apart
from macOS. The difference is in who supplies the virtual machine's
devices.

`rosgd-vz` was written first, and the development tools are built
around it. QEMU is used for the release because anyone can install it
from Homebrew, and it needs nothing of ours to be signed or notarised.
QEMU is GPL software, so it is not shipped with BOX: the user installs
it separately (`brew install qemu`), and BOX itself stays under its own
licences.

---

## 2. The machine QEMU builds

QEMU's `virt` machine is a plain ARM computer with virtio devices,
which is exactly what BOX's kernel is configured for. The launcher asks
for:

| Device | What BOX uses it for |
| --- | --- |
| virtio-gpu | the screen |
| virtio keyboard and tablet | the keyboard, and an absolute pointer |
| virtio-net, on QEMU's NAT | the network: DHCP, then everything RISC OS does |
| virtio-sound | sound, to the Mac's output |
| virtio-9p | the Host disc: a folder on the Mac |
| virtio console | the box's console, in the Terminal window |

The kernel and the ROM are handed to QEMU directly (`-kernel`,
`-initrd`), so there is no disc image and no boot loader. The box is
up in a few seconds.

---

## 3. What had to change

Three things needed work. None of them was in RISC OS itself.

### The share

Under `rosgd-vz`, the Host disc arrives over virtio-fs. QEMU on a Mac
cannot offer virtio-fs, because the daemon behind it runs only on
Linux. QEMU does offer 9P, which the Intel box already used. The fix was
one line in `/init`: try virtio-fs, and if that fails, mount the share
over 9P. HostFS then sees the same Linux directory either way.

### The screen

BOX normally expects its host to read the screen memory directly.
QEMU's window shows only what the guest says has changed, so a plain
start gave a black window. BOX already had a mode for this, made for
Apple's framework: it converts the screen and tells the GPU what has
changed, 50 times a second (`rosgd.display=convert`). With that, the
desktop appears.

### Retina and HiDPI screens

QEMU's window shows one pixel of the virtual display for each pixel of
the Mac's screen. On a Retina screen, or an external screen used at
HiDPI, a 1280 × 800 display therefore made a window only 640 × 400
points across, half the size it should be.

The launcher now gives QEMU a display twice the desktop's size,
2560 × 1600, and BOX scales its 1280 × 800 desktop up to fill it, as it
does on a 4K PC screen. The window is then 1280 × 800 points, and sharp.
The scale is worked out from the Mac's main display: its real
resolution divided by the size it "looks like". The pointer, which is
absolute, covers the same area as the window, so clicks land where
RISC OS's pointer is.

---

## 4. The mouse and keyboard

A three-button mouse works as on RISC OS: left is Select, middle is
Menu, and right is Adjust. QEMU passes the buttons through as they are,
and BOX's Input module maps them to RISC OS's buttons. This was checked
by sending the window real macOS clicks for each button and reading the
box's input log.

A Mac's own mouse or trackpad has fewer buttons. For those, BOX's Input
module treats Ctrl with a left click as Menu and Cmd with a left click
as Adjust, and Ctrl-Cmd-Delete as Break. These are BOX's own rules,
chosen for the Mac and checked under Apple's framework.

QEMU's window takes the pointer when it moves over the window. The
first click into the window may only bring it to the front.

---

## 5. The network

QEMU's NAT gives the box an address of its own on a private network,
and the box reaches the internet through the Mac. NetSurf loads secure
web pages as it does anywhere else.

The Mac cannot reach the box's own address on that network. Where the
box is to be reached from the Mac, the launcher forwards ports on the
Mac's own address (127.0.0.1): port 2222 to the box's SSH server, and
1445 to its SMB server. In the release, SSH is off unless the user
turns it on (`BOX_SSH=on`), and then only the user's own key may log
in.

---

## 6. Starting and stopping

The release is a folder: `Start BOX.command`, a `system` folder with
the kernel and ROM, and a `RISCOS` folder, which is the Host disc.
Double-clicking `Start BOX.command` opens a Terminal window for the
box's console, then the RISC OS window.

To stop BOX, use the desktop's Shutdown, or close the RISC OS window.
Both end QEMU cleanly.

There is one known fault, in QEMU rather than BOX. QEMU 11.1's window
code can crash as QEMU exits, if a keyboard or mouse event reaches the
window while it is being taken down. It is most likely when QEMU is
stopped by a signal rather than in the ways above. It happens only after
RISC OS has stopped, so it cannot harm the disc: everything saved is
already in the Mac folder.

---

## 7. How well it runs

On a Mac Studio with an M4 Max:

* The box's own self-test passes 1761 of 1761 checks under QEMU.
* The desktop is up a few seconds after the window opens.
* `!Mandel` draws its Mandelbrot set in 0.40 seconds on three Worker
  threads, one for each processor the desktop is not using.
* NetSurf starts and loads secure web pages, and the Filer, the Host
  disc and the applications on it open as they do under Apple's
  framework.

In both hosts the processor is the Mac's own, through the same
hypervisor, so there is no reason for QEMU to be slower. The two have
not yet been timed against each other.

---

## 8. Getting it

The release, *BOX for Apple silicon Macs*, is on this repository's
[Releases page](https://github.com/albanread/Aldershot/releases/tag/BOX-arm64-mac-2026-10-03).
The [translation page](../translation/README.md) gives the steps:
install QEMU with Homebrew, unzip the download, and double-click
`Start BOX.command`.
