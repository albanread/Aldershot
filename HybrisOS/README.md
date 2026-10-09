# HybrisOS

HybrisOS, also called the HAL, is a small operating system kernel that runs BOX in place of Linux. It is written for one machine only: QEMU's `virt` machine, run with the Mac's own hypervisor on Apple silicon.

This folder describes HybrisOS, and [src](src) holds its source, under the MIT licence.

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

## Related work

HybrisOS follows a similar principle to OSv and other container and library operating systems: build only the parts of an operating system that the program it runs actually uses, and leave the rest out. It is inspired by those ideas.

- OSv runs a single Linux application on a hypervisor, and provides the Linux interfaces that the application needs.
- Unikraft builds a kernel for each application from only the components that application uses.
- gVisor implements the Linux system call interface for containers, as a kernel of its own.

HybrisOS applies the same thinking to one program, BOX. It keeps the ideas BOX needs from a full operating system, such as processes, threads and several cores, and leaves out everything else.

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
- It is less secure. It has a simple permission model, a simpler memory model and none of Linux's hardening, and it has had no independent security review. See Code analysis, below, for the checks that have been made.
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

## Code analysis

The source has been checked with these tools:

| Tool | Checks | Result |
|---|---|---|
| Clang's static analyser (LLVM 23) | core, unix, deadcode, security, nullability, portability | 6 reports: 1 real fault, and 4 places tidied |
| clang-tidy | the bugprone, cert and clang-analyzer checks | no faults beyond those above; the rest are style |
| cppcheck 2.21 | warning, performance, portability | 1 report, a false positive |
| flawfinder | risk level 3 and above | nothing |
| semgrep | its C rule set | nothing |
| clang with extra warnings | -Wshadow, -Wcast-align, -Wimplicit-fallthrough, -Wnull-dereference and others | no faults; two small tidies |

The checks found one real fault, which is now fixed: the Unix socket calls took a socket from its table without checking it. A socket closed during a call meant a null pointer in the kernel, and the calls now return EBADF.

Four other reports pointed at code that was correct but depended on something the analyser could not see, such as an error code always being negative. That code now sets its values explicitly.

The rest of the reports were checked by hand and are not faults. Most were Clang's general advice against `memcpy` and `memset`, and style checks such as mixing signed values with bitwise operators.

These checks do not make HybrisOS secure. They find some kinds of fault in single files; they do not find design faults, races between cores, or faults in how the kernel checks what programs pass to it.

## Source

[src](src) holds the source, as it is built in BOX's tree:

- `boot.S`, `vectors.S`, `fpsimd.S` and `link.ld`: start-up, the exception vectors and the memory layout.
- `syscall.c`, `signal.c` and `proc.c`: the system calls, signals and the `/proc` files.
- `thread.c`, `smp.c`, `timer.c` and `trap.c`: threads, the cores, the timer and exceptions.
- `vm.c`, `mm.c`, `process.c` and `load.c`: memory, address spaces, processes and program loading.
- `ramfs.c`, `vfs.c` and `ninep.c`: the RAM file system and the shared folder.
- `socket.c`, `unix.c` and `net.c`: sockets and the network.
- `virtio.c`, `gpu.c`, `ramfb.c`, `sound.c`, `uart.c` and `tty.c`: the devices and terminals.
- `stats.c` and `vdso/`: the statistics call and the vDSO.
- `hal.h` and `hal.mk`: the declarations and the build rules.
- `lwip/`: the configuration and the few C library headers that lwIP needs.

It is built with Clang and ld.lld, as part of BOX's build (`make hal`). lwIP 2.2.1 is not included here: BOX's build fetches it, and it keeps its own BSD licence.

## Licence

HybrisOS is released under the MIT licence. See [src/LICENSE](src/LICENSE).

Copyright (c) 2026 Alban Read.

## Status

HybrisOS runs the BOX desktop with its applications, the network, sound, the shared folder, the C compiler and the SSH server, on up to eight cores. It passes BOX's self-test apart from the SMB server and Mojo, which it does not provide.

It is an experiment, and it is fun to write.
