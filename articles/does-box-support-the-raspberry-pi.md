# Does BOX Support the Raspberry Pi?

*3 October 2026*

## Introduction

No. BOX runs on Apple silicon Macs, Intel Macs and PCs. There is no
build for the Raspberry Pi, and none is planned.

That is not because the Pi is a poor machine for RISC OS. The opposite
is true. This article explains why the Pi 4 is better served by RISC OS
itself, and why BOX does not chase the Pi 5.

---

## 1. The Pi 4 already has the right RISC OS

RISC OS 5 runs natively on the Raspberry Pi 4, and runs very well. Its
Cortex-A72 processor runs 32-bit ARM code directly, so RISC OS runs
exactly as it was written:

* the kernel, the Window Manager and BASIC as hand-written ARM
  assembler, tuned over thirty-odd years;
* every RISC OS application, including those whose source is long lost;
* the inline assembler, and every program that uses it.

The result is a quick, responsive desktop on a cheap, quiet, low-power
machine. The A72 is snappy, and RISC OS on a Pi 4 is a great
experience. Nothing BOX does would improve it.

RISC OS 5.30 on the Pi 4 is also the reference BOX is measured
against: our test farm runs it, under emulation, to decide what BOX
should do.

---

## 2. BOX on a Pi 4 would be slow

In principle BOX could be built for the Pi 4. Its A72 can run 64-bit
code, and BOX already has an arm64 build for Apple silicon. But it
would be the wrong thing to do.

* **BOX gives up what the Pi does best.** On a Pi 4, RISC OS runs ARM
  code natively. BOX would replace that with RISC OS translated to C and
  compiled again: the same work, done less efficiently.
* **Translated code is large.** The translated parts of RISC OS are
  five to eight times the size of the ARM code they replace (see *BBC
  BASIC in Translation* and *The Native Wimp*). An M4 or a modern PC
  hides that with large caches and fast memory. The Pi 4's A72 has a
  48 KB instruction cache and 1 MB of level 2 cache shared by all four
  cores, and it would not hide it.
* **Linux costs something too.** BOX needs a Linux kernel underneath,
  with its own memory and its own work. Native RISC OS needs none.
* **The Pi would lose its programs.** BOX cannot run ARM binaries (see
  *Is BOX an Emulator?*). On a Pi 4, every RISC OS program already
  runs. Under BOX, many would not.

So on a Pi 4 BOX would be slower than the system it replaced, and able
to run less. Native RISC OS is the better choice there, and is likely to
remain so.

---

## 3. The Pi 5

The Raspberry Pi 5 is a different case. Its Cortex-A76 processor runs
32-bit ARM code only for applications, not for an operating system, so
RISC OS 5 cannot run on it natively. BOX, being 64-bit, could in
principle be built for it.

But the Pi 5 is not where BOX is aimed. It is no longer the cheap
option it once was, and there is now a wide range of small, low-cost
"mini PCs" that cost about the same or less, and do better:

* faster processors, and more memory;
* proper storage (NVMe or SATA) rather than an SD card;
* a case, a power supply and cooling included;
* standard PC hardware, which Linux already supports in full, so BOX
  runs on them from a USB stick with no special work.

For BOX, a mini PC is the better machine for the money. Every hour
spent supporting the Pi 5's particular hardware would buy less than the
same hour spent on BOX itself.

---

## 4. Summary

| Machine | Recommended |
| --- | --- |
| Raspberry Pi 4 | RISC OS 5, natively. It is fast, and runs everything. |
| Raspberry Pi 5 | Not supported by BOX. A mini PC gives more for the money. |
| PC or mini PC | BOX, booted from a USB stick |
| Mac | BOX, or an emulator for software BOX cannot run |

The Pi 4 and RISC OS are a fine pair, and BOX is not trying to replace
them. BOX is for the machines that RISC OS cannot run on natively.
