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

A first **preview release: early days BOX, x64**. It boots RISC OS from a
**USB stick on a 64-bit PC**, with a few applications to try — for people
who like being close to the machinery. Not emulated. Native.

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

A hero shot of BOX is coming soon.

---

## Downloads

Coming soon. The first preview, **early days BOX x64** — booting from a
USB stick on a 64-bit PC — is described [above](#what-runs-today); its
downloads will appear on this repository's
[Releases](https://github.com/albanread/Aldershot/releases) page.

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
