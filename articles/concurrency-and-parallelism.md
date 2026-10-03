# Concurrency and Parallelism

*3 October 2026*

## Introduction

RISC OS was written for a single processor, with no memory protection
between programs. Its desktop runs many programs at once, but only one
runs at any moment, and each one decides when to let the others have a
turn. This is co-operative multitasking.

BOX runs RISC OS on top of Linux, on processors with many cores. This
article describes:

* the difference between concurrency and parallelism
* how BOX runs each task as a Linux thread, and what the tasks share
* the baton, which passes from task to task at `Wimp_Poll`
* why the task threads are kept on one core
* the personality lock, which stands in for "interrupts off"
* where BOX's real parallel work happens: the Linux helpers
* why RISC OS tasks cannot simply run in parallel
* how native modules and applications can still use the other cores.

---

## 1. Two words

| Word | Meaning |
| --- | --- |
| Concurrency | several things in progress over the same stretch of time, taking turns. One cook with three pans on the stove is concurrent. |
| Parallelism | several things running at the same instant. Three cooks with a pan each are parallel. |

RISC OS programs are concurrent: they take turns. BOX keeps them that
way. Parallelism happens in BOX too, but only in helpers that work
outside RISC OS, as section 6 describes.

---

## 2. Every task is a thread

When you start an application, the Wimp makes it a task. In BOX every
task is a Linux thread, and all the threads live in one Linux process.
They share one block of memory, the **arena**, which holds the memory
map of 32-bit RISC OS (see *BOX Architecture*):

| Address | Area | Belongs to |
| --- | --- | --- |
| `&00008000` | the application slot | the running task |
| `&60000000` | the RMA | everyone: modules' workspace |
| `&70000000` | the system heap | everyone |
| `&78000000` | dynamic areas | everyone: the font cache, sprites and more |
| `&B4000000` | screen memory | everyone |
| `&FC000000` | the ROM | everyone |

The shared parts are shared by every task, as on RISC OS. Only a few
things belong to one task:

* its application slot, mapped at `&8000`;
* its SVC stack;
* its registers, stack pointer and error handlers;
* its floating point state;
* a few words in zero page: its domain and its environment handlers.

---

## 3. The baton

Only one task thread runs at a time. The rest sleep. The right to run
is called the **baton**.

A task gives the baton away when it calls `Wimp_Poll`, the call every
desktop program makes to ask whether there is anything for it. The Wimp
decides which task runs next, exactly as on RISC OS. The handover goes
like this:

1. The task calling `Wimp_Poll` puts the next task's world in place:
   that task's slot is mapped at `&8000`, its SVC stack goes back where
   it was, and its registers and zero page words are restored.
2. It wakes the next task's thread.
3. It goes to sleep until the baton is handed back.

When the next thread wakes, everything is exactly as it left it, so it
simply returns from its own `Wimp_Poll` with its event.

Nothing fights over the baton. It is handed on deliberately, at
`Wimp_Poll`, so a program that never calls `Wimp_Poll` holds up the
whole desktop. This is the same as on RISC OS.

---

## 4. All on one core

Because only one task thread can run at a time, BOX keeps all the task
threads on the same processor core. Waking a thread on another core is
slower: on the Apple silicon box a task switch took about 58
microseconds that way, against about 6 on the same core. More cores
would sit idle while making every switch slower.

Spreading the tasks across cores would not break anything, because the
baton and the lock make sure each thread sees what the previous one
wrote. It would just be slower.

---

## 5. The personality lock

On RISC OS, modules protect themselves by turning interrupts off while
they change something shared. Much module code is not re-entrant: it
must never run twice at once.

BOX has no interrupts to turn off. Instead it has one lock, the
**personality lock**. Any thread that runs module code holds it, so
module code never runs twice at once. The task holding the baton holds
the lock while it runs.

Real RISC OS also does work behind the scenes in interrupts: tickers,
events, keyboard and network buffers filling. In BOX this is called
**background work**. It runs holding the lock, at safe points:

* when a task waits, for example in a slow system call;
* when a SWI returns to the program that called it;
* inside the calls that wait for input, such as `INKEY`.

So background work can happen while an application calculates or
waits, just as an interrupt would. It never cuts into the middle of
module code.

The baton and the lock are different things. The baton decides which
task runs. The lock decides who may run module code: the task holding
the baton, or background work at a safe point.

---

## 6. Where the parallelism is: the helpers

Real parallel work in BOX happens only in Linux helper threads. Some
wait on network sockets, some on timers, some feed the sound device,
some read the keyboard and mouse. They run on any core, at the same
time as everything else.

They keep one strict rule: **a helper never runs RISC OS code**. It
does its job on the Linux side, then posts a note to an event queue,
such as "a packet arrived" or "the sound buffer needs filling". It
needs no lock to do that. The RISC OS side picks the note up at its next
safe point and handles it, as it would an interrupt.

| Side | What runs |
| --- | --- |
| RISC OS | one thing at a time, in turns: tasks pass the baton at `Wimp_Poll`, and background work runs in the gaps |
| Linux | helpers, in parallel on any core, which never touch RISC OS directly and only report back |

---

## 7. Why tasks cannot simply run in parallel

Three things stop two RISC OS tasks from running at the same instant:

* **Every task's memory is at the same address**, `&8000`. Threads in
  one Linux process share one view of memory, so two tasks cannot both
  have their slot there at once. Parallel tasks would need separate
  processes.
* **Module state is global.** The RMA, the Wimp's windows, FileSwitch,
  the font cache and the rest were written for one processor. Two tasks
  making system calls would mostly wait for each other at the lock
  anyway.
* **RISC OS programs expect to be left alone** between their
  `Wimp_Poll` calls. They assume no other program changes a file, a
  system variable or the screen while they are working.

So co-operative multitasking is not a limitation BOX has added. It is
what RISC OS software is built on, and BOX keeps it.

---

## 8. Using more cores

### Native modules

A module written natively in C for BOX can start its own Linux threads,
for decoding images, rendering pages, compressing archives or network
transfers. Those threads follow the helper rule:

* no SWIs;
* no touching the RMA or other module state;
* no writing into memory a running program might be looking at.

When the work is done, the thread posts the result, and the module
finishes the job under the lock at a safe point. This is how a hardware
driver works on real RISC OS: the device works on its own, and the
driver finishes in its interrupt handler.

### Applications: Workers

The **Worker module** offers the same kind of threads to applications.
A program submits a job, a kernel and its arguments, and a pool of
threads, one for each core the tasks do not use, runs the jobs in
parallel while the desktop carries on. The workers may write only into
memory that stays mapped whatever task runs. `Worker_ShareMemory` maps
part of a program's own memory a second time so that they can write
there too. A finished job bumps a pollword, and the Wimp hands the
program event 13. Programs can even supply their own kernels, which the
module checks before it runs them.

`!Mandel`, in Apps, shows it: the Mandelbrot set drawn by seven workers
about 6.7 times as fast as by one, while the desktop stays usable. See
*Workers and the Mandelbrot* for the whole story.

For an ordinary application nothing changes. Call `Wimp_Poll` often,
don't hold the machine for long, and let BOX do the rest.

---

## 9. Summary

* Each task is a Linux thread. All tasks share one memory arena laid
  out like 32-bit RISC OS.
* One task runs at a time. The baton passes at `Wimp_Poll`, chosen by
  the Wimp, as on RISC OS.
* Task threads stay on one core: more cores would only make task
  switches slower.
* The personality lock stands in for "interrupts off". Background work
  runs under it at safe points, like interrupts.
* True parallelism belongs to Linux helpers, which never run RISC OS
  code and report back through an event queue.
* Native modules use more cores the same way, and applications use them
  through the Worker module.
