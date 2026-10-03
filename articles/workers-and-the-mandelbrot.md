# Workers and the Mandelbrot

*3 October 2026*

## Introduction

RISC OS runs on one processor. Its desktop is co-operative: one task
runs at a time, and hands over to the next when it calls `Wimp_Poll`.
BOX keeps that model exactly, and so all of a desktop's tasks share one
core of a machine that may have ten or more.

**Workers** let a RISC OS program use the other cores. A program hands
the Worker module a piece of computation, a **job**, and carries on. A
pool of Linux threads runs the jobs in parallel, outside RISC OS
altogether, and the program is told when they finish. The results land
in memory the program can read, including its own application memory.

This article describes:

* why RISC OS tasks cannot simply run in parallel
* what a worker is, and the rule that keeps it safe
* jobs, kernels, and how a task learns that a job has finished
* sharing a task's own memory with the workers
* application kernels: code a program supplies itself, and how it is
  checked
* `!Mandel`, the demonstration, and what it shows.

Workers are RISCOSGrandDesign design 28. The Worker module is native C,
in the ROM of both BOX machines.

---

## 1. Why tasks cannot run in parallel

Every Wimp task in BOX is a Linux thread, and all of them live in one
process, sharing the arena: the low 4 GB, laid out like 32-bit RISC OS
(see *BOX Architecture*). Only one task thread runs at a time. The right
to run, the **baton**, passes at `Wimp_Poll`, and the Wimp chooses who
gets it, as on RISC OS. The task threads are all kept on one core:
since only one can run, more cores would only make each task switch
slower.

Three things stop two RISC OS tasks running at the same instant:

* **Every task's memory is at the same address.** Each task's
  application slot is at `&8000`, and only the running task's slot is
  mapped there. Threads in one process share one view of memory, so
  two tasks cannot both have their slot in place at once.
* **Module state is global.** The RMA, the Wimp's windows, FileSwitch
  and the font cache were written for one processor. They protect
  themselves by turning interrupts off, which in BOX becomes a single
  lock. Two tasks calling SWIs would mostly wait for each other.
* **Programs expect to be left alone** between their `Wimp_Poll` calls.
  No other program changes a file, a system variable or the screen
  while they work.

Co-operative multitasking is not a limitation BOX has added. It is
what RISC OS software is built on, and BOX keeps it.

---

## 2. What a worker is

BOX already has threads that run in parallel: Linux helpers that wait
on network sockets, timers, the sound device, the keyboard and mouse.
They run on any core, and they keep one rule: **a helper never runs
RISC OS code**. It does its work on the Linux side and posts a note to
an event queue, which RISC OS picks up at its next safe point, much as
it would handle an interrupt.

A worker is a helper that runs computation. The Worker module keeps a
pool of worker threads, one for each core the tasks do not use. They
are not tied to the tasks' core, never take the lock, and never call a
SWI. Each runs a **kernel**, a function that computes, over memory that
stays mapped whatever task holds the baton:

* the job's record, in the RMA;
* the job's buffers, in the RMA, a dynamic area, or a shared window
  (section 5);
* the job's pollword (section 4).

That is the whole safety argument. A worker can never see a task's slot
change underneath it, because it never touches `&8000`.

---

## 3. Jobs and kernels

A task submits a job with `Worker_Submit`, giving:

* the kernel to run;
* up to sixteen argument words, which are copied, so they may be
  anywhere;
* a pollword, or none;
* a tag of its own, to recognise the job by.

`Worker_Submit` checks every buffer address before the job is queued,
and returns a handle: the address of the job's record in the RMA. The
task goes on with its work, or returns to `Wimp_Poll`, while a worker
runs the job.

The other calls are what you would expect:

| SWI | Does |
| --- | --- |
| `Worker_Submit` | queues a job |
| `Worker_Status` | reads a job's status: queued, running, done, failed or cancelled |
| `Worker_Result` | collects a finished job's eight result words |
| `Worker_Cancel` | takes a job off the queue, or stops it running |
| `Worker_Completed` | returns the next finished job, in the order submitted |
| `Worker_Info` | the pool's size, and a limit on how many threads to use |

A small result, such as an iteration count, a checksum or a total, comes
back in the job's eight result words. A large one, such as a row of
pixels, is written into a buffer the job was given.

The module has one kernel of its own, `mandel_row`, which computes one
row of the Mandelbrot set into 32 bpp sprite pixels. Section 6 describes
how a program supplies its own kernels.

---

## 4. Telling the task: the pollword

A task should not sit in a loop asking whether its jobs are done. RISC
OS already has a way for something outside a task to wake it: the
**pollword**. A task gives `Wimp_Poll` the address of a word, and asks
for event 13, *Pollword_NonZero*. When the word becomes non-zero, the
Wimp gives the task that event.

A worker that finishes a job adds one to the job's pollword. It never
calls RISC OS to do it, so it needs no lock. The task gets event 13 on
its next `Wimp_Poll`, collects its finished jobs with
`Worker_Completed`, and sets the word back to zero.

The order is guaranteed. A worker writes its results first, then
publishes the job's status, and only then increments the pollword. The
Wimp reads the pollword with an acquire load. So a task that has seen
event 13 sees every result of the jobs that caused it.

---

## 5. Sharing a task's own memory

A program's data is normally in its own slot, at `&8000` and up. A
worker cannot write there directly, because whichever task holds the
baton has its slot at that address, and it may not be the program that
asked.

`Worker_ShareMemory` solves this. In BOX, each slot is a Linux memory
object (see *The Native Wimp*). A memory object can be mapped more than
once, so the Worker module maps the shared range a second time, at a
**window** in the dynamic-area part of the arena, which never moves.
The workers write through the window. The pages are the same, so the
results appear at the program's own addresses, with no copy, even while
another task holds the baton.

From BBC BASIC:

```
DIM raw% size% + 8 : area% = (raw% + 3) AND NOT 3
SYS "Worker_ShareMemory", 0, area%, size% TO win%
REM jobs write at win% + n; the program reads area% + n
...
SYS "Worker_ShareMemory", 1, win%
```

A few rules keep this safe:

* **A shared range cannot be shrunk away.** While a window exists, the
  slot cannot shrink below it, whether by `Wimp_SlotSize`, `*WimpSlot`
  or a flex heap. Growing is allowed.
* **A buffer belongs to its jobs** until they have finished. The program
  should not read a row until the job that writes it is done.
* **Unsharing cancels the jobs that use the window**, so nothing is
  left writing into memory that has gone.
* **When a task ends**, by closing down or otherwise, its jobs are
  stopped and its windows removed before its memory goes.

---

## 6. Application kernels

A built-in kernel only goes so far. A program can supply its own with
`Worker_LoadKernel`. There are two ways to make one:

* **in C**, compiled by clang and linked by `roscc link --kernel`, which
  writes a small kernel file. roscc refuses static data, calls to
  anything outside the kernel, and absolute addresses;
* **in BBC BASIC's assembler**, which in BOX assembles for the host
  processor (see *BBC BASIC in Translation*). The program assembles
  the kernel into a `DIM` block and passes it to the Worker module as
  it is.

A kernel is a C function on the machine BOX runs on:

```
uint32_t kernel(const uint32_t *args, uint32_t *results,
                const volatile uint32_t *cancel);
```

It returns 0 when it has finished, and it should look at its cancel word
now and then. Each worker runs it on a 64 KB stack of its own, and puts
every register back afterwards, whatever the kernel did.

### The checker

`Worker_LoadKernel` copies the code into the RMA, makes it read-only,
and then checks it. It refuses a kernel that:

* makes a system call or calls a SWI;
* touches system registers;
* uses the register that holds BOX's static base (`x18` on Apple
  silicon, the `%fs` and `%gs` prefixes on Intel);
* jumps through a register, or jumps or refers outside itself.

Because the pages are read-only before the check, the code that runs is
the code that was checked.

At run time, a kernel that faults fails its own job, and the box
carries on. A kernel that ignores its cancel word is stopped 250 ms
after it has been asked to stop, so a runaway kernel cannot hang the
desktop.

The checker cannot follow arithmetic. A kernel can still work out an
address past the end of its buffers and write there. The checker stops
mistakes and the obvious escapes, but it is not a sandbox: a kernel is
trusted code, as a module is.

---

## 7. The Mandel demonstration

`!Mandel` is in the Apps directory of BOX's disc. Double-click it, and
it draws the Mandelbrot set, 960 by 600 pixels, while the desktop goes
on working.

It works as a real application would:

1. It makes a 32 bpp sprite in its own memory, a BASIC `DIM` block, and
   shares the sprite's pixels with `Worker_ShareMemory`.
2. It submits one job for each of the 600 rows. Each job writes its row
   straight into the sprite, through the window.
3. It returns to `Wimp_Poll` with a pollword. On each *Pollword_NonZero*
   event it collects the finished rows and redraws just those, so the
   picture fills in as the rows arrive.
4. When the last row is in, it shows the time taken, the number of
   workers, and the total computing time the rows used.

At start-up it also assembles its own `mandel_row` kernel in BASIC, for
whichever processor BOX is running on, and loads it with
`Worker_LoadKernel`.

| Key or button | Does |
| --- | --- |
| Select | zoom in on the point clicked |
| Adjust | zoom out |
| `W` | one worker, or all of them |
| `K` | the module's kernel, or Mandel's own |
| `+` and `-` | more or fewer iterations |
| `H` | the whole set again |
| `R` | recalculate |

### What it shows

Timed on the Apple silicon box, with eight processors (seven workers),
960 by 600 pixels:

| Picture | Iterations | 7 workers | 1 worker |
| --- | --- | --- | --- |
| The whole set | 4,096 | 0.17 s | 1.2 s of computing |
| The whole set | 65,536 | 2.56 s | 17.08 s |
| After one zoom | 65,536 | 5.50 s | 36.12 s |

Seven workers are about 6.7 times as fast as one. The work divides
almost perfectly, because rows are independent and a worker takes the
next row as soon as it finishes one. Writing straight into the
program's own memory costs nothing measurable: the whole set took 2.54
seconds that way, against 2.56 seconds through a dynamic area.

Mandel's own kernel, assembled by BASIC, is as fast as the module's:
2.57 seconds against 2.56 at 65,536 iterations. Its picture shows bands
of colour where the module's is smooth, because it colours from a
256-entry palette rather than a continuous ramp.

While it computes, the desktop stays usable: windows can be opened and
dragged, and closing Mandel mid-picture stops its jobs cleanly.

---

## 8. Summary

* RISC OS tasks run one at a time, and BOX keeps them that way.
* Workers are Linux threads that run computation on the other cores.
  They never run RISC OS code, and touch only memory that stays mapped.
* A job runs a kernel over its buffers, and the task learns it has
  finished through its pollword.
* `Worker_ShareMemory` lets workers write straight into a task's own
  memory, through a window onto the same pages.
* Programs can supply their own kernels in C or BASIC assembler. The
  Worker module checks the code before it runs, and a kernel that
  faults fails only its own job.
* `!Mandel` shows it all: about 6.7 times faster with seven workers, and
  a desktop that stays responsive while it computes.
