# HybrisOS

HybrisOS, also called the HAL, is a small operating system kernel that runs BOX in place of Linux. It is written for one machine only: QEMU's `virt` machine, run with the Mac's own hypervisor on Apple silicon.

This folder describes HybrisOS. Its source is not published here yet.

## Overview

BOX is RISC OS translated to C. Its first program, `/init`, is the RISC OS personality: the kernel's SWIs, the modules, the Wimp and the desktop. On every release so far `/init` has run on a Linux kernel.

Linux is a very large dependency. The kernel that BOX boots is a 7.6 MB image, built from a source tree of tens of millions of lines, and BOX uses only a small part of what it offers.

HybrisOS replaces it with less than 20,000 lines of original code:

| Part | Lines |
|---|---|
| HybrisOS: C and AArch64 assembly, with comments | about 11,400 |
| The same, without comments and blank lines | about 9,600 |
| lwIP 2.2.1, the TCP/IP stack (third-party, unchanged) | about 24,800 |

Built, HybrisOS is a 215 KB image.

## Why it can be this small

HybrisOS is only possible because it targets QEMU's `virt` machine. That machine is designed to be simple, and real hardware is not. Its devices are virtio devices: the screen, the keyboard and tablet, the network, sound and the shared folder are each a set of queues in memory, all driven in the same way. There are no clocks or power domains to set up, no firmware to talk to, no USB controller and no SD card. The interrupt controller (GICv3), the timer and the serial port are standard ARM parts, described in a device tree that QEMU supplies.

The second reason is that HybrisOS supports only the system calls that BOX uses. It does not try to be Linux. It answers the calls that `/init` and the programs it starts (clang, ssh, sshd and the rest) actually make, about 140 of them, and refuses any other with ENOSYS.

## The BOX boundary

BOX talks to the kernel only through Linux system calls, and the boundary between BOX and the kernel is the same on both. HybrisOS implements those calls, so the same `/init` binary runs on Linux and on HybrisOS without change.

HybrisOS adds four calls of its own, for the screen, sound, screen updates and its statistics. Linux refuses these with ENOSYS, and `/init` uses whichever kernel answers.

## What it supports

- Several processes, each with its own address space: fork, vfork, execve and wait, and static ELF programs.
- Threads (clone), futexes, signals and the timers that musl and the box use.
- Several cores, up to eight, with one lock for the kernel itself.
- Pipes, pseudo-terminals, Unix sockets, and TCP and UDP sockets through lwIP.
- A RAM file system for the root, and the Mac's shared folder over 9P, which is BOX's HostFS disc.
- The virtio screen, keyboard, tablet, network and sound devices.
- A vDSO, so that reading the clock does not need a system call.

It supports processes and threads because working without these basic operating system ideas would be unbearable. BOX's C compiler, its SSH server and its desktop's helper programs all need them.

## What it costs

HybrisOS is a smaller and younger kernel than Linux, and it is worse in several ways:

- It is less well tested. Linux has decades of use on millions of machines; HybrisOS has BOX's own self-test and the programs its author has run.
- It is less secure. It has a simple permission model, a simpler memory model and none of Linux's hardening, and it has had no security review.
- It is less performant in some respects. One lock covers the whole kernel, so system calls on different cores wait for each other. The shared folder has no cache, and the kernel waits for QEMU while a file operation completes.
- It lets you do less. BOX on Linux has the WebKit browser, the SMB server (`*Share`), and Mojo, which needs Linux's dynamic linker. HybrisOS has none of these.
- It runs BOX on fewer systems: Apple silicon Macs only, under QEMU with the Mac's hypervisor.

## Apple silicon only

HybrisOS targets Apple silicon Macs, using QEMU. The box's processor is the Mac's own, through the hypervisor, so BOX's code runs at full speed. Other machines are not supported.

It would probably be very hard to port HybrisOS to real hardware such as a Raspberry Pi 4 or 5. A port would need drivers for the SD card or eMMC, the USB host controller and the devices behind it, Ethernet, the display through the VideoCore firmware, the clocks and power, and the GIC-400 interrupt controller. Most of these are far more complex than the virtio devices HybrisOS drives today.

## Tasks and SVC mode

On RISC OS, an application runs in user mode (USR), and the operating system runs in supervisor mode (SVC). An application calls the operating system with a SWI, which enters SVC mode. An application that touches the operating system's memory gets an error such as "Abort on data transfer".

On Linux, the whole of BOX runs as one user-mode program. The RISC OS personality and its applications all run at the same privilege level, so an application can overwrite the operating system's memory without any fault.

On HybrisOS, the RISC OS personality runs at EL1, the processor's privileged level, and RISC OS applications run at EL0, the unprivileged level. This is the arrangement RISC OS has with SVC and USR mode. An application reaches the operating system only through the SWI gate page, much as a SWI enters SVC mode. If an application touches the operating system's memory, it gets its own RISC OS error, and BBC BASIC's assembled code is treated in the same way. So tasks may behave more like RISC OS tasks on HybrisOS than they do on Linux.

## Efficiency

HybrisOS may be more efficient than Linux for BOX, because it does only what BOX needs. Some measurements, taken on the same Mac; the Linux figures cover both QEMU and Apple's Virtualization framework:

| | HybrisOS | Linux |
|---|---|---|
| Switching between two RISC OS tasks | 3.5 to 4 µs | 8.5 to 9 µs |
| The same, both tasks held on one core | 2.5 to 3.3 µs | 6.1 to 6.5 µs |
| The same, the tasks on two cores | 10 to 12 µs | 14.4 to 15.1 µs |
| Kernel image | 215 KB | 7.6 MB |

These are early figures from BOX's self-test. They do not show that HybrisOS is faster overall, and the single kernel lock and the uncached shared folder will make it slower for some work.

## Status

HybrisOS runs the BOX desktop with its applications, the network, sound, the shared folder, the C compiler and the SSH server, on up to eight cores. It passes BOX's self-test apart from the SMB server and Mojo, which it does not provide.

It is an experiment, and it is fun to write.
