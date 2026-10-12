# Dynarmic in the BOX

*12 October 2026*

## Introduction

BOX runs programs that exist only as ARM machine code in its ARM
container. *The Emulator in the BOX* describes the container from the
outside: what it is for, why only the processor is emulated, and what
runs in it. This article goes underneath, to the engine at its centre,
**dynarmic**, and to the code that joins it to RISC OS.

It covers:

* what dynarmic is, which copy BOX takes, and why BOX keeps its own
  mirrors of it;
* the two back ends, and what the A32 front end translates;
* how the engine's memory is BOX's memory;
* the flow of execution: entering ARM code, SWIs, calls in both
  directions, safe points and the end of a program;
* where ARM code outside an application comes from;
* code that changes, and how translations are thrown away;
* processor modes and privileged instructions;
* floating point;
* errors and faults;
* ARM modules and shadows;
* performance and the diagnostics;
* building the engine and updating it;
* the limits and the open issues.

The code is in A7232ToolChain, under `rosgd/runtime/armrun`. File and
function names are given so that a reader can find it. The design is
design 25 of the Grand Design, *ARM binaries*.

---

## 1. What dynarmic is

dynarmic is a dynamic recompiler for ARM code. It was written for
the Citra emulator of the Nintendo 3DS, and later used by yuzu. It
translates a block of guest ARM instructions into the host processor's
own code the first time the block runs, keeps the translation in a code
cache, and runs the translation from then on.

Inside, it has three layers:

| Layer | What it does |
| --- | --- |
| Front ends | Decode guest code into dynarmic's own intermediate form (IR). There is an A32 front end (ARM and Thumb, ARMv3 to ARMv8 AArch32, with VFP and NEON) and an A64 one. |
| Optimiser | Passes over the IR of a block: constant folding, dead code elimination, the elimination of redundant register reads and writes, and others. |
| Back ends | Emit host code from the IR: x86-64, AArch64 and RISC-V (not built for BOX). |

It is a **user-mode** engine. It models the registers, the flags, VFP,
memory accesses and the exclusive monitor. It does not model processor
modes, banked registers, interrupts or an MMU. Its embedder gives it
memory and answers its calls for anything outside user mode. In BOX
that embedder is the bridge (`bridge.c`), described in the rest of this
article.

### Which copy

The original repository was archived in March 2024. BOX takes the fork
maintained by the Azahar project, the successor to Citra, at
`github.com/azahar-emu/dynarmic`. It is pinned to one commit:

| Component | Commit | Licence |
| --- | --- | --- |
| dynarmic (Azahar's fork) | `a46601580d`, 26 September 2026 | 0BSD |
| mcl (utilities) | `5fc4beaf33` | MIT |
| oaknut (the AArch64 assembler) | `94c726ce03` | MIT |
| xbyak (the x86-64 assembler) | `8c0098b69f` | BSD-3-Clause |
| robin-map (hash tables) | `054ec5ad67` | MIT |
| fmt (formatting) | `7b273fbb54` | MIT |
| zydis, zycore (x86-64 disassembly, for debugging only) | `bffbb610cf`, `0b2432ced0` | MIT |
| Boost 1.89.0, headers only (`icl`, `variant`) | release tarball, SHA-256 checked | BSL-1.0 |

Nothing in it is GPL or LGPL. `runtime/armrun/VENDOR.md` is the record
of the pin, the licences and the changes.

### Why BOX keeps its own mirrors

Since 4 October 2026 the build fetches dynarmic and each of its
externals from private mirrors on GitHub: `albanread/dynarmic`, and
`albanread/dynarmic-fmt`, `-mcl`, `-oaknut`, `-robin-map`, `-xbyak`,
`-zydis` and `-zycore`. The Boost tarball is a release asset,
`boost-1.89.0`, on `albanread/dynarmic`. BOX depends on dynarmic, so
it preserves a copy. BOX's own changes to it are kept as seven patches,
described where they matter in sections 3, 6 and 7. The patches are
published, with a note of the pin and how to apply them, at
[github.com/albanread/Aldershot/tree/main/src/box/thirdparty/dynarmic](https://github.com/albanread/Aldershot/tree/main/src/box/thirdparty/dynarmic).

A clean build from the mirrors gave libraries byte for byte the same
as one from upstream. `DYNARMIC_UPSTREAM=1` builds from upstream
instead.

---

## 2. The two back ends

BOX runs on two kinds of machine, and dynarmic has a back end for each.
Each build of the engine has only the back end its machine needs.

| Box | Host processor | Translation | Assembler |
| --- | --- | --- | --- |
| Apple silicon (VZ) | AArch64 | A32 to A64 | oaknut |
| Intel (QEMU/HVF, or a PC) | x86-64 | A32 to x86-64 | xbyak |
| The hosted test on the Studio | AArch64 | A32 to A64 | oaknut |

Only the A32 front end is built (`DYNARMIC_FRONTENDS=A32`). The engine
is configured in `armrun.cpp` (`armrun_create`) as ARMv8 AArch32,
always little-endian, with one exclusive monitor per engine.

### What the A32 front end covers

ARMv8 AArch32 is a superset of every architecture RISC OS binaries were
built for, so everything a 32-bit RISC OS program uses decodes: the
data processing, multiply, load and store instructions of every era,
SWP, LDREX and STREX, VFP and NEON. A survey of the 107 `&FF8` images
on the test disc (`runtime/armrun/A2-RESULTS.md`) found every reached
instruction in the SharedCLibrary clients translatable, except FPA.

The exceptions are:

* **FPA.** The old floating point instructions are coprocessor
  instructions on CP1 and CP2. dynarmic has no FPA. BOX supplies one,
  natively (section 8).
* **Privileged instructions.** CPS, RFE, SRS, `LDM`/`STM` with `^`,
  and `MSR` writing the control field are outside a user-mode engine.
  BOX's patches make the engine hand them to the bridge (section 7).
* **26-bit code.** An AIF image whose header says 26-bit is refused by
  the loader (`ros_armrun_run`, `box.c`) with "is 26-bit ARM code,
  which ROSGD does not run".
* **Thumb.** RISC OS programs are ARM code, and design 25 refuses
  Thumb. ARM code that branches to an odd address (`BX`, `BLX`, or a
  load into the PC) sets the T bit, and the engine would follow it into
  Thumb. It is not given the chance: `armrun.cpp` refuses to fetch any
  instruction for a location whose T bit is set, so the block is a
  single fault at its start, and the bridge raises "undefined
  instruction" naming the address (section 9). No Thumb instruction is
  ever translated or run. The bridge clears the T bit whenever it sets
  the CPSR, so the engine runs ARM code again afterwards.

---

## 3. Memory

### The arena is the guest's memory

In BOX every RISC OS address is a host address. The box's `/init`
holds the RISC OS memory map in the low 4 GB of its own address space,
the **arena**: the application slot at `&8000`, the RMA, the dynamic
areas, the screen, zero page, the SVC stack (see *ROMs, Modules and the
Memory Map*).

dynarmic has a mode for exactly this, called **fastmem**. Its
`fastmem_pointer` is the host address of a 4 GB space laid out as the
guest's memory should be, and every translated load or store becomes a
host load or store at that base plus the guest address. In the box the
arena's origin is 0 (`ros_arena_base`, `include/rosgd/arena.h`), so
`armrun_create` passes 0, and a guest address *is* the host address.

So ARM code reads and writes RISC OS's memory directly. A sprite area,
a Wimp block or a file name passed to a SWI is the same memory the
native code reads. Nothing is copied or translated between the two.
In the hosted test the arena is a reservation somewhere else in the
Mac's memory, and the pointer is its base.

### When an access faults

Fastmem has no checks. An access to an address with nothing mapped
there makes the host processor fault, and the fault is how dynarmic
finds out:

1. The host raises SIGSEGV (or SIGBUS). In the box the runtime owns
   those signals for the whole process (`runtime/fault.c`). Its
   handler first asks the engine, through `ros_fault_jit`, whether the
   fault was a guest access in translated code
   (`armrun_handle_fault`, which calls dynarmic's
   `dynarmic_handle_fault`).
2. If it was, dynarmic rewrites the signal context so that the access
   goes to its slow path instead, and the handler returns. If it was
   not, the runtime handles the fault as its own.
3. The slow path calls the engine's memory callbacks, which are the
   bridge's `on_read` and `on_write` hooks. They ask the runtime
   whether the range is memory (`box_valid`, `ros_arena_valid`). If it
   is, the access is made with an ordinary copy. If not, the bridge
   records a data abort and calls `armrun_abort`.
4. dynarmic also marks the instruction "do not fastmem" and throws the
   block away (`recompile_on_fastmem_failure`, on by default). Its next
   translation makes that one access through the callbacks every time,
   so a faulting instruction faults once.

Exclusive accesses (`LDREX`/`STREX`) always go through the callbacks,
which BOX implements as a compare and store (`MemoryWriteExclusive*`
in `armrun.cpp`). Instruction fetches go through a third hook,
`on_fetch`, which is asked only while a block is being translated.

### Patch 0001: the runtime owns the signals

Upstream, the first engine created installs its own SIGSEGV handler,
chains to the previous one, and gives the creating thread a 2 MB
alternate signal stack. In the box that would replace the task
thread's signal stack and put dynarmic in front of the runtime's
handler. BOX's patch 0001 builds dynarmic with
`DYNARMIC_EXTERNAL_SIGNALS`, which installs nothing and exports
`dynarmic_handle_fault` for the runtime to call. The hosted build
keeps dynarmic's own handler.

### Exact data aborts: patch 0006

A RISC OS program's error handler expects the PC of a data abort to be
the instruction that made the access, with the registers as they were.
dynarmic offers this as `check_halt_on_memory_access`: after a slow-path
access, the translated code tests the halt word and stops the block if
the access was aborted. BOX turns it on (`armrun_halt_on_access`).

On AArch64 the check loaded the 32-bit, 4-aligned halt word with a
64-bit `LDAR`, which needs 8-byte alignment, and faulted inside the
translated code. Patch 0006 makes it a 32-bit `LDAR`, as every other
load of that word is.

The check is emitted only on the slow paths, so ordinary accesses pay
nothing on AArch64. On x86-64 the option also turns off dynarmic's
get/set elimination pass, which costs the Intel box 38% of engine time
(section 11). We decided on 4 October 2026 to keep exact aborts on
both boxes. The boot option `rosgd.armnohalt` turns them off, for
measurement only.

### Known differences from RISC OS 5.30

| Case | RISC OS 5.30 on a Cortex-A72 | The engine |
| --- | --- | --- |
| Unaligned `LDR` or `LDRH` | abort on data transfer | loads the unaligned value and goes on |
| A load whose value is never used | aborts if the address is not memory | the optimiser removes the load, so it cannot abort |

---

## 4. The flow of execution

### Starting an ARM application

`*Run` of an `&FF8` file reaches FileSwitch (`fsutils.c`). If the file
is a C application's image for BOX, `ros_capp_run` takes it. If not, it
is ARM code, and `ros_armrun_run` (`box.c`) loads it as RISC OS does:

1. It reads the file's header, refuses 26-bit images, and works out
   the memory needed (an AIF's read-only, read-write and zero-init
   sizes, or an Absolute's length).
2. It sets the memory (`ros_capp_memory_for`), calls StartApplication
   (`OS_FSControl 2`) with the command line, and loads the file at
   `&8000`.
3. It runs the program as the task's application
   (`ros_module_run_as_application`, `run_arm`), on the task's own
   thread, the one the native Wimp made.

`run_arm` creates an **ARM task** (`arm_task_create`, `bridge.c`) over
the arena, with the runtime's services in an ops table: the SWI
dispatcher, the environment handlers, the memory check, the gate to
native code, the safe point, the native-code test and the fault report.
The task starts in USR mode with R12 and R13 at `&80000000`, R15 at
`&8000`, and R14 at a marker address (below). An AIF image's own header
code then decompresses, relocates and zero-initialises the image, as
ARM code like the rest.

### Engines, tasks and threads

An ARM task owns a small stack of engines, one for each depth of
nesting, up to eight (`ARM_DEPTH`). Each engine is a separate dynarmic
`Jit`, with its own registers and its own code cache: 16 MB for depth
0 and 8 MB for each nested engine. They are made when first needed.

Engines are never shared between threads. Each Wimp task runs on its
own thread in BOX, so an ARM application's engines see only that
task's application slot. A thread with no ARM application, which still
has ARM code to run (an ARM module's entry, a vector claimant), makes a
**task for calls only** (`system_task`, `arm_task_create_calls`) the
first time, kept until the thread ends.

### The loop

The bridge's `loop` runs the engine with a budget of 500,000
instructions (`BUDGET`), about a millisecond of ARM code:

```
for (;;) {
    armrun_run(engine, BUDGET)       dynarmic's Run
    count the instructions; sample the PC if the budget ran out
    deliver an FPA trap or a data abort, if one is waiting
    install new registers, if a hook asked for them
    a safe point: the runtime's background work (box_poll)
}
```

dynarmic counts instructions by block, so the budget is approximate.
The hooks never change the engine's registers while a block runs. A
hook that decides the program must go somewhere else (an error handler,
an exit handler, a return from a callback) records the registers it is
to have and halts the engine. The loop installs them when `Run`
returns.

### SWIs

An `SVC` instruction ends its block and calls the bridge's `on_svc`
with the number, R15 already past the instruction. A few SWIs are the
bridge's own, because they concern the emulated processor:

| SWI | What the bridge does |
| --- | --- |
| OS_Exit | enters the application's exit handler, or ends the task |
| OS_GenerateError | without X, enters the error handler; with X, returns V set |
| OS_EnterOS | changes the modelled mode to SVC, with its banked registers |
| OS_LeaveOS | changes it back to USR |
| OS_SynchroniseCodeAreas | throws translations away (section 6) |

Every other SWI goes to `box_swi`, always in its X form. `box_swi`
copies the registers into a `struct ros_cpu` and calls
`ros_swi_as(..., ROS_KIND_ARM)`, the same dispatcher native code uses,
on the same thread. The caller's kind matters for modules that have an
ARM shadow (section 10). If the SWI fails and the program did not ask
for the X form, the bridge delivers the error to the program's error
handler, as the kernel would. A native error never unwinds through
dynarmic's own stack frames: errors always come back to the bridge as
values.

When the ARM code is in a privileged mode, its frames may be on the SVC
stack below the runtime's own idea of the stack top (`ros_svc_sp`).
`box_swi` then moves `ros_svc_sp` below the ARM code's R13 for the
length of the SWI, so that native code called by the SWI does not
write over those frames.

### Native code calling ARM code

Native code calls ARM code through `ros_call`, as it calls any code at
an address. `ros_call` (`runtime/dispatch.c`) tries compiled code, then
C applications' and modules' code, then `ros_armrun_call`, which
accepts the address if it is ARM code (`is_arm_code`): in a registered
range (section 5), or in the running ARM application's memory.

`ros_armrun_call` runs the code on the next engine of the thread's
task, the ARM application's if there is one, otherwise the calls-only
task (`arm_task_call`). The call's R14 is set to the marker address
`&FFFFFFF0`. Fetching that address gives the engine the instruction
`SVC &FFFFFF`, and that SVC, in `on_svc`, means "the call has
returned". The nested `loop` then ends, and the registers go back to
the native caller.

This is how ARM modules' entries run, how vector claimants and
transient callbacks in ARM code are entered, and how a SWI made by ARM
code can call back into ARM code (a `qsort` comparator in
SharedCLibrary, for example) while the engine below is still inside
that SWI.

### ARM code calling native code (#146)

ARM code reaches native code by SWIs, and also by branching to an
address that is native code: a callback that a native program handed
to an ARM module, for example. Before #146 such a branch fetched the
native machine code and ran it as ARM.

Now, when the engine translates an address, `on_fetch` asks the
runtime whether the address is native (`box_native`): compiled code,
a native entry, or a C program's or module's code, including native
code assembled by BASIC. ARM ranges and the ARM application's memory
are tested first and stay ARM. A native address is translated as
`SVC &FFFFFE`, the gate. In `on_svc` the bridge takes the address from
R15 - 4 and calls `box_gate`, which makes the call with `ros_call` and
the ARM registers, inside `ROS_TRY`. The ARM code goes on at the R15
the native code left, normally its R14. An error raised in the native
code stops at the gate and is delivered as a non-X SWI's error would
be.

The native code may call ARM code again, on the next engine, and so on,
up to eight engines deep. If the address stops being native, the gate
finds that out at the call, throws its own translation away and lets
the address be fetched again as ARM.

The range `&FFE00000` to `&FFE10000` is reserved for **gateways**:
addresses that would take an ARM caller straight to a native fast path,
for example a native `memcpy` for the emulated SharedCLibrary. The
bridge supports them, but none is in use yet (`box_gate` returns 0 for
them).

### Safe points and background work (#156)

Each time the budget runs out, and after a halt, the loop reaches a
safe point. There the runtime's queued background work runs
(`box_poll`, `ros_safe_point`): keys, Escape, ticker events, and a
TaskWindow's time slice. This is what makes an ARM program that never
calls a SWI still respond to Escape, and share time in a task window.

Issue #156 was a fault here. Background work starts on the SVC stack
at `ros_svc_sp`. When privileged ARM code had been entered without a
SWI to lower that point (SharedCLibrary's `LibInit`, called from an
application's SWI in USR mode, starts at the very top of the SVC
stack), the work wrote over the ARM code's live frames. OvationPro
failed to start about one time in 36. `box_poll` now passes the ARM
code's R13 when it is in a privileged mode, and the work starts below
it, as an interrupt's handlers do on RISC OS.

### How a program ends

A program ends by OS_Exit, by returning to the R14 it was given (as
from an Absolute: OS_Exit with R0 to R2 zero), or by an error with no
handler of its own. If the program has set its own exit or error
handler, the bridge enters it in USR mode, as the kernel does. If the
handler is the system's default, which is native, the bridge ends the
task instead, recording the return code or the error.

`run_arm` then destroys the task and its engines, and only after that
calls the runtime's OS_Exit or OS_GenerateError for the program. The
runtime's unwinding, a long jump back to the command that ran the
program, therefore never crosses dynarmic's stack frames.

---

## 5. Where the code comes from

Some ARM code is not an application at `&8000`. The container keeps a
list of **ARM ranges**, address ranges in the RMA or elsewhere that hold
ARM code (`ros_armrun_code_add`, `box.c`). There are 256 entries.

A range in application space belongs to the slot that was mapped at
`&8000` when it was registered, and is seen only while that slot is
mapped (#262). Each Wimp task has its own slot, and two tasks' `DIM`med
blocks often sit at the same addresses, so a range there is that
task's alone: another task assembling over the same addresses neither
sees it nor forgets it. When a slot is destroyed its ranges go with
it, and when a task takes over an ended task's space they move to it
(`ros_armrun_slot_moved`). Ranges anywhere else, in the RMA or the
ROM, are everyone's. The runtime's list of BASIC's assembled native
code (`capp.c`) is kept the same way.

| Code | How it becomes an ARM range |
| --- | --- |
| An ARM module loaded from a file | `place_module` (`runtime/module.c`) unsqueezes it if squeezed (#162), then registers its image |
| A RISC OS 5.30 ROM module wrapped by `tools/romwrap.py` | copied into the RMA, its address words and its branches into SharedCLibrary moved, then registered as above |
| The emulated SharedCLibrary | copied from the 5.30 ROM image, 1,336 words relocated (`ros_armrun_builtin_clib`) |
| A utility (`&FFC`) | loaded into an RMA block and registered for the length of the call (`ros_armrun_utility`) |
| BBC BASIC's assembled ARM code | registered at the closing `]` (`basicasm_ket`) |

### Squeezed modules (#162)

Many modules on a RISC OS disc are squeezed by `modsqz`: the
initialisation offset has bit 31 set, and the code is compressed.
RISC OS 5's kernel unsqueezes such a module before it starts it. BOX
did not, and called the squeezed word as code ("Call to &E0D441CC,
which is not compiled code"). `ros_module_unsqueeze` now does what the
kernel's `UnSqueeze` does, before the image is registered.

### BASIC's ARM assembler

BOX's BBC BASIC assembles the host's native code by default. A program
written for ARM is recognised: with no `*BasicAsmCPU`, a block that
reads as ARM (R0 to R15, `#`, register lists, `LDM`, `SWI`, or a
mnemonic the native encoder does not know) is assembled as ARM from
its start (#178 made this look at the whole block first). At `]` the
block is registered as an ARM range, and anything previously
registered over it in the same application space, ARM or native, is
forgotten. `CALL` and `USR` then
reach it through `ros_call`, as does an ARM module given its address.
See *BBC BASIC in Translation*.

---

## 6. Code that changes

dynarmic translates code once, and assumes the code stays as it was.
When code changes, the translations of it must go.

RISC OS has had a rule for this since the StrongARM: a program that
writes code calls OS_SynchroniseCodeAreas before running it. An ARM
program's call reaches the bridge's `on_svc`, which calls
`arm_task_synchronise_all`: in every ARM task, every engine throws away
its translations of the range (`armrun_invalidate`, dynarmic's
`InvalidateCacheRange`), or all of them if no range was given
(`armrun_clear`). All tasks are told, because the RMA's code is shared
by all of them.

dynarmic does not do this at once. It records the range, under a lock,
and requests a halt. The engine running it stops at the end of its
current block and throws the translations away before its next `Run`.
This is safe from any thread.

The runtime invalidates by itself whenever the meaning of an address
changes:

* when an ARM range is added, removed or forgotten (a module loaded,
  killed or replaced, a BASIC block reassembled);
* when native code is registered over an address
  (`ros_armrun_code_native`, from `ros_capp_code_add`), so that a
  branch there is translated as a gate from then on (#146).

A new application at `&8000` gets a new ARM task with new engines, so
nothing translated for a previous program survives.

A program that writes over its own code without calling
OS_SynchroniseCodeAreas runs the old translation, as it could run old
instructions from the instruction cache of a StrongARM. The 107
programs of the corpus all synchronise. Those that are squeezed call
it twice while their own decompressor runs.

Native code that writes ARM code calls the same SWI. That reaches the
native handler (`runtime/capp.c`), which looks after the host's own
instruction cache and then tells the engines in the same way
(`ros_armrun_synchronise`): every engine of every task, on every
thread, throws away its translations of the range, or all of them.

On AArch64 the native handler cleans the instruction cache only over
pages that are resident. A resident page can still be inaccessible (a
page of the RMA made `PROT_NONE`), and the cache maintenance faults on
it; such a page is passed over, since it holds no code that can run.
Before this was found, OS_SynchroniseCodeAreas for all of memory from
native code could end in "abort on data transfer" in the runtime.

### Patch 0002: invalidate every block a range covers

dynarmic keeps each translated block's address range in Boost ICL's
`interval_map`, and finds the blocks that a range covers with
`equal_range`. ICL's containers sit on `std::map` with overlap as
equivalence, and libc++, which BOX uses on both hosts, stops at the
first overlapping node it meets. So an invalidation over four blocks
threw away one of them, and the other three ran their old code.

Patch 0002 replaces the block index with a `std::multimap` from each
block's first address to its last, searched from (the range's start
minus the longest block), which no ordering can make ambiguous. It
erases the blocks it invalidates, which upstream did not. The pending
ranges become a plain vector. The self-test rewrites four chained
blocks and checks that one invalidation retranslates all four.

---

## 7. Modes and privilege

### The engine is user mode only

dynarmic runs everything as user mode. RISC OS code is not all user
mode: a module's SWI handler runs in SVC mode, error and exit handlers
are entered in USR mode from SVC, callbacks run in SVC mode, and
SharedCLibrary switches modes itself. The bridge models what dynarmic
does not:

* **The mode** RISC OS would be in (`t->mode`). It is also written into
  the CPSR's mode bits, so that `MRS` reads it correctly, though the
  engine ignores them.
* **The banked registers**: R13, R14 and the SPSR of USR, SVC, UND and
  IRQ modes (`bank13`, `bank14`, `spsr`). FIQ mode is not modelled,
  since no FIQ code runs in BOX. `switch_mode` swaps R13 and R14 when
  the mode changes.
* **The flags** cross between the engine's CPSR and `struct ros_cpu` as
  N, Z, C, V, Q and the I bit. The user-mode CPSR is `&110` (USR, with
  the A bit set), as RISC OS 5.30 gives on the A72.

### Instructions handed to the bridge: patches 0003 and 0007

Upstream, dynarmic translates CPS, RFE, SRS, `LDM`/`STM` with `^`, and
`LDM ^` with the PC as an "Interpret" terminal. dynarmic has no
interpreter: the AArch64 back end aborted the whole process on them,
and the x86-64 one called a fallback. **Patch 0003** raises them instead,
as a new last exception, `InterpretRequired`, on both back ends.

An `MSR` that writes the CPSR's control field (the mode and the
interrupt masks) was translated by upstream as user mode treats it: the
write was dropped. SharedCLibrary returns to user mode with
`MSR CPSR_c`, so the bridge never saw it go. **Patch 0007** raises
`InterpretRequired` for those too. An `MSR` of the flags alone is
still translated by the engine.

The bridge's `privileged` function executes what is raised. It tests
the condition itself (the engine hands some over before testing it),
and then handles:

| Instruction | In a privileged mode | In USR mode |
| --- | --- | --- |
| CPS | the interrupt masks and the mode | no effect |
| MSR CPSR | flags, masks and mode | the flags only |
| MSR SPSR, MRS SPSR | the mode's SPSR | undefined instruction |
| `LDM`/`STM` with `^` | the user bank's R13 and R14 | undefined instruction |
| `LDM ^` with PC | return, CPSR from the SPSR | undefined instruction |
| `MOVS`, `SUBS`, `ADDS`, `MVNS` into the PC | return, CPSR from the SPSR | (translated by the engine) |

RISC OS 5.30 gives "undefined instruction" for every one of them in
user mode, which the bridge copies. RFE and SRS are not handled and
give "undefined instruction" in any mode. A shifted operand in a
`MOVS pc` form is not handled either; none has been seen.

### Undefined instructions and coprocessors: patch 0004

With no coprocessor installed for CP1 and CP2, every FPA instruction
made both back ends abort the process ("Should raise coproc exception
here"). **Patch 0004** gives the translator the list of installed
coprocessors. It asks the one an instruction names whether it takes
the instruction. If none does, the instruction raises
`UndefinedInstruction` when its condition passes, as the hardware does.

Undefined, unpredictable and undecodable instructions all reach the
bridge's `on_exception`. Unless `privileged` takes them, they become
RISC OS's "undefined instruction" error.

### Patch 0005: single-stepping after a raise

`Jit::Step` runs one instruction. Stepping an instruction that raised
an exception set the block's terminal twice, which dynarmic asserts
against. Patch 0005 sets the step terminal only when none is set.
Ordinary runs were never affected. The differential test harness steps
through code, and so does debugging.

---

## 8. Floating point

### FPA, natively

Most C programs for RISC OS use the FPA floating point instructions,
which on RISC OS 5 the FPEmulator module emulates by trapping each one
as an undefined instruction. BOX does not run an FPEmulator. It
installs two dynarmic coprocessors, CP1 and CP2 (the `Fpa` class in
`armrun.cpp`). As each FPA instruction is translated, the coprocessor
rebuilds the instruction word from the fields the decoder gives it, and
emits a direct call to `fpa_execute` (`fpa.c`). The call is
compiled into the translated block, so no trap is taken.

`fpa_execute` works on the task's `struct ros_fp`, which holds the
eight FPA registers as doubles and the FPSR. The native C library uses
the same state.

| Group | Instructions |
| --- | --- |
| Arithmetic | ADF MUF SUF RSF DVF RDF POW RPW RMF FML FDV FRD POL; MVF MNF ABS RND SQT LOG LGN EXP SIN COS TAN ASN ACS ATN URD NRM |
| Transfers | FLT FIX WFS RFS WFC RFC |
| Compares | CMF CNF CMFE CNFE, into the ARM flags |
| Memory | LDF STF in single, double, extended and packed decimal; LFM SFM |

The arithmetic is double precision, rounded to single where the
instruction asks for single. Extended precision is held and computed as
double, so an extended result has 53 bits of mantissa, not 64; it is
stored in the 96-bit extended format. The rounding modes in the
instruction are honoured through the host's rounding mode. WFC is
ignored and RFC reads 0, since there is no emulator to control.

Exceptions follow the FPSR. The cumulative flags are set from the
host's IEEE flags. If the program has enabled the trap for an exception
(C programs enable invalid operation, division by zero and overflow),
the result is FPEmulator's error, `&80000200` plus the exception's
number, "Floating point exception: ...".

A coprocessor call has no program counter and cannot stop its block.
So a trapped exception is raised when the block ends: the bridge
ignores the block's later SWIs and exceptions, but its stores still
happen, and the error's PC is the end of the block. On RISC OS the PC
would be the FPA instruction. This is a known difference.

### VFP and NEON

VFP and NEON are translated by dynarmic directly into the host's
floating point and vector instructions. Their registers live in each
engine (`armrun_ext`). On the processor there is one register file,
which a program shares with the callbacks it is called into, a
`qsort` comparator for example. The bridge keeps it so: a nested call
(section 4) starts with the caller's 32 double registers and FPSCR, and
when it returns the caller goes on with them as the callee left them
(`vfp_carry`, `bridge.c`). A callee therefore sees the program's
rounding mode, and one that keeps d8 to d15, as the procedure call
standard asks, keeps them for its caller. Transient callbacks and
handlers entered on the depth-0 engine use its registers, as they
would on RISC OS, where such code must not use VFP without a context
of its own. The corpus has 12 VFP instructions and no NEON at all.

---

## 9. Errors and faults

### The errors

The bridge gives the errors RISC OS 5.30 gives (`bridge.h`):

| Event | Error number | Text |
| --- | --- | --- |
| An undefined, unpredictable or untranslated instruction | `&80000000` | Internal error: undefined instruction at &xxxxxxxx |
| A branch into Thumb code (section 2) | `&80000000` | Internal error: undefined instruction at &xxxxxxxx (Thumb code, which ROSGD does not run) |
| A fetch from no memory, or `BKPT` | `&80000001` | Internal error: abort on instruction fetch at &xxxxxxxx |
| A load or store to no memory | `&80000002` | Internal error: abort on data transfer at &xxxxxxxx |
| A trapped FPA exception | `&80000200` to `&80000204` | Floating point exception: ... |

Each is delivered to the program's error handler as OS_GenerateError
would deliver it: the handler's buffer gets the PC, the number and the
text, and the handler is entered in USR mode with R0 its private word.
Inside a nested call, every call above ends with the error, and the
error is delivered as the engines unwind, by the engine at depth 0.

For a task for calls only there is no application to deliver to. The
calls end, and the error comes back to the native caller of the ARM
code with V set, as a module's entry returning an error would.

If the program has no handler of its own, it ends. A Wimp task then
gets RISC OS's "may have gone wrong" box, and the desktop and other
tasks carry on. *The Emulator in the BOX* describes the fault drill
that proves this.

### The fault report

Before a fault is raised as an error, the bridge calls `box_fault`,
which writes a report to the serial console. It names the module and
offset of the PC and of R14, the access that was refused, the modelled
mode, the nesting depth and the Wimp task, and gives the registers and
the SVC stack above R13. #156 added it, because the error text alone
names only the PC. This one is from #165:

```
armrun: abort on data transfer at &605B041C (ABCLibrary+&28CC); reading 4 bytes at &4C434241
armrun:   an application, USR mode, depth 0, task &100; R14 in ABCLibrary+&27DC
armrun:   R0  &0000F388  R1  &0000F2D4  R2  &00006FF0  R3  &00000000
armrun:   R4  &00008188  R5  &0000F864  R6  &00000000  R7  &00000000
armrun:   R8  &00008004  R9  &0000FC8C  R10 &0081A901  R11 &0081A901
armrun:   R12 &4C434241  R13 &6E206269  R14 &605B032C  R15 &605B041C
armrun:   CPSR &60000110
```

---

## 10. ARM modules and shadows

### Modules in the chain

An ARM module loaded from a file (`*RMLoad`, OS_Module 1) is
unsqueezed if need be, left where it was loaded in the RMA, registered
as an ARM range, and joins BOX's module chain like any other, with its
`arm` flag set. Its entries (initialisation, finalisation, service
calls, SWIs, `*` commands) are called through `ros_call`, so they run
on the calling thread's engines. `*NModules` marks such a module `A`
(see *Monitoring the Machine*).

### Shadows (#147)

RISC OS has one module per title. If BOX followed that rule, an ARM
program's `RMLoad` of, say, ZLib would replace the native ZLib, and
every native caller would be sent into emulated code. We decided on
4 October 2026 that native code uses an ARM module only
when there is no native one, and that ARM code may **shadow** a native
module.

When an ARM module of a native module's title loads, the native
module's **shadow policy** (a column in `boot/rom_contents.c`) decides:

| Policy | Modules | What happens |
| --- | --- | --- |
| Allow | MimeMap, Squash, ZLib, CompressPNG, CompressJPEG, BootCommands, BASICVFP, ColourPicker | the ARM module becomes the native module's shadow |
| Built-in | SharedCLibrary | 5.30's library is made as the shadow at the first ARM reference |
| Never (the default) | the kernel, FileSwitch, the Wimp, the drivers and the rest | the ARM module is not loaded ("absorbed"), and the native module serves ARM callers too |

A module loaded from a file takes its ROM title's policy, or is
shadowable if its title is in `ROSGD$Shadowable`.

The caller's kind routes everything (`route`, `ros_module_for_swi_kind`
in `module.c`). An ARM caller's SWIs, `*` commands and OS_Module
lookups reach the shadow. A native caller's reach the native module.
Service calls are divided into those that notify, provide or are
withheld; a shadow's claims on TickerV, EventV and UpCallV are called
after every native claimant, and a shadow may not claim GraphicsV; and a shadow initialising may not change system variables that
already exist. `*ARMPrefer <title> on` sends native callers to the
shadow too, for testing. Design 25 §12 has the whole of it.

### SharedCLibrary

ARM C programs call SharedCLibrary in the APCS-32 standard, with
doubles in pairs of integer registers, variadic functions and pointers
to ARM functions. BOX's native library cannot take such calls without a
marshalling layer for every entry. So ARM programs get RISC OS 5.30's
own SharedCLibrary 6.23, cut from the 5.30 ROM
(`runtime/armrun/arm/SharedCLibrary,ffa`). At the first ARM reference
`ros_armrun_builtin_clib` copies it into the RMA, moves the 1,336 words
that point into it (it is linked at `&FC182320`), registers it as ARM
code and initialises it through `ros_call`. Only ARM callers reach it.
The same mechanism lets 5.30's ROM C modules, wrapped by
`tools/romwrap.py`, call into the emulated library.

---

## 11. Performance and diagnostics

### The figures

These were measured on 4 October 2026 with `tests/armrun/bench.py`
(medians of five). The Apple silicon box is a Mac Studio M4 Max, the
Intel box a Mac Pro Xeon W-3235.

| | md5sum, 16 MB | djpeg, a 2560-pixel JPEG to a PPM |
| --- | --- | --- |
| Apple silicon, 4 vCPUs | 99 ms (engine 85, native 12) | 430 ms (engine 131, native 295) |
| Apple silicon, `rosgd.armnohalt` | 100 ms (engine 83) | 446 ms (engine 128) |
| Intel, 4 vCPUs | 3121 ms (engine 373, native 2660) | 4020 ms (engine 502, native 3410) |
| Intel, `rosgd.armnohalt` | 2943 ms (engine 232) | 3779 ms (engine 310) |
| The host's own code | 19 ms | 13 to 20 ms |

They show:

* On the M4, md5sum takes about 5.4 times as long as the host's own
  md5. djpeg's computation takes 6 to 10 times as long as
  libjpeg-turbo with its SIMD code.
* The x86-64 back end, on a 2019 Xeon, is about 4.4 times slower than
  the AArch64 one on the M4, and exact aborts cost it a further 38%.
  On AArch64 they cost nothing measurable.
* The number of vCPUs, from 2 to 8, makes no difference: an ARM task
  holds the baton like any task, and its engine is one thread. Moving
  the ARM task to a core of its own (`rosgd.armcpu=1`) made djpeg 37%
  slower, because its file I/O then crossed cores.
* What a user waits for is mostly the native time, and most of that is
  file I/O through the share; on the Intel box the share costs about
  645 µs an OS_GBPB (#144).
* The emulated SharedCLibrary is not hot: 6% of md5sum's samples. Its
  native fast paths (section 4, gateways) wait for a program where it
  is.
* In the desktop, one AWViewer render of the ArtWorks start-up picture
  took 24 ms of ARM code and 47 ms of the native code it called.

### Where the time goes

`*ARMStats` divides each thread's time between three places: the engine
(running ARM code, or translating it), native code that ARM code called
(SWIs and gates), and the native FPA. The bridge switches place with
`at_go` around every SWI, gate and FPA instruction, and reads the
monotonic clock at each switch. It also samples the PC each time the
budget runs out, about once a millisecond of ARM code, and names the
module and offset of the busiest 16-byte ranges. The Desk Meter's ARM
window shows the same counters live. *Monitoring the Machine* describes
both, with real output.

The clock reads are cheap only because of the vDSO. On the Apple
silicon box, until the kernel's vDSO was linked with
`--hash-style=both` on 11 October 2026, musl could not find
`clock_gettime` in it, and each read was a real system call. That cost
every SWI in the box, ARM or native, not only the container's.

### Tools for finding faults

* **`*SWITrace`** records SWIs from ARM code like any other, with the
  caller's kind.
* **The fault report** (section 9) on the serial console.
* **`make armrun-smoke`** and **`make armrun-hello`** run the engine and
  the bridge on the Mac, without a box (`tests/armrun/smoke.c`,
  `hello.c`). On the Mac Pro `make armrun-hello` tests the x86-64 back
  end natively.
* **`make armscan`** runs the corpus tools: images unsqueezed by their
  own decompressors under the engine, every word probed, idioms run.
* **`tests/armrun/idioms.py`** runs 36 cases of old instruction idioms
  under `Run`, under `Step`, and on RISC OS 5.30 on the farm, and
  compares them.
* **The self-test** (`boot/selftest_armrun.c`, `selftest_shadow.c`)
  covers a block and its SVC, a fault in translated code, invalidation
  over four blocks, `*Run` of an ARM program with its handlers, a
  utility's error, nesting native and ARM calls four deep each way,
  OS_SynchroniseCodeAreas from native code, VFP across a nested call,
  the refusal of Thumb, ARM and native ranges in two application
  spaces, and the shadow rules.

---

## 12. Building and updating the engine

### The build

`deps/build-dynarmic.sh` builds the engine. The Makefile calls it when
the library is missing, or when the script or a patch is newer.

| `TARGET` | Output | Compiler |
| --- | --- | --- |
| `aarch64-linux-musl` | `.cache/aarch64/dynarmic-musl` | zig 0.15, static, `-fPIE` |
| `x86_64-linux-musl` | `.cache/dynarmic-musl` | zig 0.15, static, `-fPIE` |
| `host` | `.cache/dynarmic-host` | the Mac's own `c++` |
| `host-x86_64` | `.cache/dynarmic-host-x86_64` | the Mac's `c++` for x86-64, to run under Rosetta |

The script:

1. fetches Boost 1.89.0's headers once, and checks the tarball's
   SHA-256;
2. clones dynarmic into `.cache/dynarmic-src` from the mirror, checks
   out the pin, fetches the seven externals it builds from their
   mirrors, and applies `runtime/armrun/patches/*.patch` in order;
3. checks that zig is 0.15 (for the box), and writes a stamp holding
   the pin and a hash of the patches, so that a change to either gives
   a clean tree;
4. runs dynarmic's CMake with Ninja: Release, tests off, the A32 front
   end only, and for the box `-DDYNARMIC_EXTERNAL_SIGNALS` and
   `-include cstdlib`;
5. merges dynarmic, fmt, mcl and (on x86-64) Zydis and Zycore into one
   archive, `lib/libarmrun-engine.a`, and copies the headers
   `armrun.cpp` needs;
6. builds beside the old output and moves the new one into place only
   when all of it is there, so a failed build keeps the old engine;
7. records in the output's `versions` file the pin, the patches' hash,
   the target and the compiler ("zig 0.15.2", or the host's `c++`).

The Makefile runs the script at every `make`. It does nothing when the
`versions` file matches what it would build, and builds again when the
pin, a patch or the compiler has changed, saying what the engine was
built from before. When it does nothing the library is not touched, so
nothing after it is remade.

`armrun.cpp` is the only C++ in `/init`, and nothing outside
`runtime/armrun` includes a C++ header. It is compiled with
`zig c++ -std=c++20` and linked with zig's libc++. The engine and its
C++ runtime add about 1.1 MB to the Apple silicon `/init`.

### Gotchas

* **zig must be 0.15** (#243). An engine built by zig 0.17 refers to
  0.17's libc++ (`std::__1::__hash_memory`) and will not link into a
  `/init` built by 0.15. The Makefile checks the version and gives the
  pinned zig to each `deps/build-*.sh` rule, and since 12 October 2026
  `build-dynarmic.sh` checks it too, and refuses another. Run by hand
  with `ZIG` unset, it takes `~/zig015/zig` when that is there, as the
  Makefile does.
* **The cache records the compiler** (since 12 October 2026). An engine
  built by another compiler, or copied in from another tree's `.cache`
  before then, has a `versions` file that does not match, and the next
  `make` builds it again. Nothing needs to be deleted by hand.
* **fmt and `<cstdlib>`** (#153). fmt's `format.h` calls `malloc` and
  `free` without including `<cstdlib>`, which zig's libc++ for musl
  does not bring in by another header. A fresh build failed until
  the script added `-include cstdlib`. Trees that already had a built
  engine never saw it.
* **The x86-64 back end is tested on the Mac Pro.** The Studio has no
  Rosetta, so `host-x86_64` builds there but cannot run.

### Updating the pin

1. Update the mirrors first: fetch the new upstream commit into
   `albanread/dynarmic`, and the commits its submodules name into each
   `albanread/dynarmic-<name>`.
2. Change `DSHA` in `deps/build-dynarmic.sh`, and Boost's version and
   hash if dynarmic now wants another.
3. Rebuild. Each patch is applied with `git apply`, which stops at the
   first that does not apply. Rework those patches against the new
   source, keeping each change marked `ROSGD (runtime/armrun/patches/000N)`.
   Drop a patch only when upstream has fixed the fault, and prove that
   with the test that found it.
4. Rebuild all three targets, and run the hosted tests, the box
   self-test on both boxes, `tests/armrun/accept.py` and the OvationPro
   desktop probe.
5. Update `VENDOR.md`: the pin, the licences of anything new, and the
   patches.
6. Publish the patches again: `tools/publish/export-box.sh` copies
   them, with their README (`docs/publish/thirdparty/dynarmic`), to
   `src/box/thirdparty/dynarmic` in the public repository.

---

## 13. Limits and open issues

* **The acceptance set.** As recorded on 4 October 2026, 8 of the 17
  binary-only applications on the test disc start; OvationPro and
  AWViewer are the large ones that work. Periodic, which faulted then,
  starts since 12 October (#165). Design 25's sprint A7 holds the list.
* **Differences** from RISC OS 5.30: unaligned loads, unused
  loads (section 3), FPA traps at the block's end, and extended
  precision held as double (section 8). We decided to leave these as
  they are: none is known to matter to a program that works on 5.30.
* **Gateways** to native fast paths are built in the bridge but none is
  used.
* **An ahead-of-time translator**, retiring the engine one application
  at a time, is design 25's last step (A8). It has not been started.

### Fixed on 12 October 2026

| Fault | Cause | Fix |
| --- | --- | --- |
| A utility's error read after its block was freed (#263) | `ros_armrun_utility` freed the utility's RMA block, which often holds its error, before copying the error | The error is copied first, up to its terminator, within valid memory |
| One task's native code unregistered another's ARM code (#262) | ARM ranges and BASIC's native ranges were keyed by address alone, but each task has its own application space | Ranges in application space belong to their slot, and go with it (section 5); the table now holds 256 |
| Native OS_SynchroniseCodeAreas did not reach the engines | The native handler cleaned the host's caches only | It tells every engine on every thread too (section 6) |
| VFP registers not carried across nested calls | Each engine had its own, and only R0 to R15 crossed | One register file across a nested call (section 8) |
| A branch to an odd address ran Thumb code | Nothing stopped the engine following the T bit | Refused at the fetch, as an undefined instruction naming the address (sections 2, 9) |
| Periodic faulted in ABCLibrary at start-up (#165) | Not the engine: BOX lacked OS_Control (SWI &0F), which ABC programs call to read their handlers; the "SWI not known" error reached a handler that never expected it | OS_Control, as the kernel's: three calls of OS_ChangeEnvironment. Periodic now opens its table |
| An engine built by the wrong zig stayed cached (#243) | The build script did not check the version, and its stamp did not record the compiler | It checks for 0.15 and records the compiler; a change rebuilds (section 12) |
