# Worker

Computation jobs run by a pool of Linux threads on the cores that the desktop's tasks do not use, while the tasks carry on. This is the BOX's own module, in native C, in the ROM of both boxes (A7232ToolChain #125).

A worker thread is not pinned to the tasks' core, never takes the personality lock and never calls RISC OS. It runs a kernel over memory that stays mapped whichever task holds the baton. The job record, its buffers and its pollword must be in the RMA, a dynamic area or a shared window. `Worker_Submit` checks every address. Application space is refused, because every slot is at &8000 and only the running task's is mapped there.

## SWIs (chunk &C0180)

All are raw. Registers not written are kept.

| SWI | Purpose |
| --- | --- |
| `Submit` (&C0180) | R1 kernel, R2 -> up to 16 argument words (R3 count), R4 -> pollword (0 for none), R5 caller's tag. Returns the job handle in R0. |
| `Status` (&C0181) | R0 job. Returns status, tag and kernel. |
| `Result` (&C0182) | R0 job, R1 bit 0 keeps the job. Returns the tag and eight result words. |
| `Cancel` (&C0183) | Dequeues a queued job, or stops a running one and waits for it. |
| `Info` (&C0184) | R0 = 0 reads the pool (threads, limit, queued, running, version 300, machine). R0 = 1 sets the thread limit. |
| `Completed` (&C0185) | Returns the next finished job not yet returned, or 0. |
| `ShareMemory` (&C0186) | R0 = 0 maps part of the caller's application memory a second time at a window. R0 = 1 removes one window. R0 = 2 removes all. |
| `LoadKernel` (&C0187), `UnloadKernel` (&C0188) | Load or remove an application's own kernel. |

Statuses are 0 queued, 1 running, 2 done, 3 failed and 4 cancelled. Errors are &C0180 to &C018F, ROSGD's own range. Their texts are in `worker.c`. They include "Worker job not finished" (&C0184) and "Worker job failed: abort on data transfer" (&C0186).

## Completion

Pass the pollword to `Wimp_Poll` (R0 bit 22, R3 -> the word) with nulls masked out. A finishing worker writes the results, writes the status with a release store, fences, and increments the pollword atomically. It wakes the Wimp's idle wait if the word was 0. The task gets event 13. It must reset the word to 0 first, then call `Completed` until it returns 0 and `Result` for each job. That order means a job finishing during collection is never missed.

## Shared windows

`ShareMemory` maps the pages of a task's slot a second time (`MAP_SHARED`) in the dynamic-area address range. A worker writing through the window writes the task's own memory, even when another task holds the baton. The slot cannot shrink below its highest window, and unsharing cancels the jobs that use it. A task's windows go at Service_WimpCloseDown, after its jobs are stopped.

## Kernels

Kernel 1 is `mandel_row`: one row of the Mandelbrot set. It takes eleven argument words (row buffer, width, maximum iterations, flags, palette, and x0, dx and y as 32.32 fixed point or doubles). Results are the iterations, the points inside and the microseconds taken.

An application can load its own kernel with `LoadKernel`. The code is copied to read-only executable pages and checked by `kcheck.c`. The kernel is a C function on the BOX's machine: `uint32_t kernel(const uint32_t *args, uint32_t *results, const volatile uint32_t *cancel)`. It returns 0 when done, &C0185 if it saw its cancel word set, or an error number. It runs on a 64 KB stack of its own. A kernel that has not stopped 250 ms after a cancel is taken away. A fault fails the job and the worker goes on.

Kernels can be built with `roscc link --kernel`, or assembled in BASIC (`!Mandel` does this).

The checker refuses system calls, system registers, x18 on AArch64, indirect branches, and branches or PC-relative references out of the kernel's own image. It guarantees that the code that runs is the code checked. It does not stop a kernel from writing any arena memory it can address. A kernel is the application's own code, trusted as the application is. Application kernels run only in the BOX, not in a hosted build (&C018F).

## The pool

There is one thread per core that the tasks do not use, or one thread on a one-core machine. The threads start at the first `Submit`. The queue is first in, first out. A task's jobs and kernels end with it.

## Demo

`disc/Apps/!Mandel` is a BBC BASIC desktop program that submits one `mandel_row` job per row through a shared window. Keys: Select zooms in and Adjust zooms out, K switches kernel, W switches between one worker and all, + and - change the iterations, H is home and R runs again.

Tests: `boot/selftest_worker.c` covers Info, results, the pollword, cancel, windows, application kernels, the checker and task close-down. `tests/worker/mandelk.c` is the application kernel it loads.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
