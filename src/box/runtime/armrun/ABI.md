# ABI.md — the ARM container's bridge (sprint A0)

The contract between the engine (A32 code, executed by dynarmic —
design 25 §7, amended 3 October 2026; first written for TCG) and the
runtime (the translated OS), as design 25 §4/§6 built it. Every
convention below is quoted from the RISC OS 5.31 kernel sources
(`riscos-src/BCM2835/RiscOS/Sources/Kernel/s/`, the BCM2835 5.31
tree); where the sources are silent the item is listed in §7 for
verification on the instrumented RPCEmu. Survey date: 30 September
2026.

## 1. The call: SWIs, forwarded unconditionally

- The engine executes A32 (ARMv8-A AArch32, ARM state; Thumb refused;
  FPCP not implemented — §4). An SVC reaches dynarmic's
  `CallSVC(imm24)` callback with R15 already past it, and the block
  ends in a halt check. The engine has the number and the register
  state; as the kernel takes it from
  `[R14_svc - 4]` (`s/Kernel:514`), the word at the SVC's address is
  readable from the arena if ever needed — the engine passes the number
  directly.
- The bridge is `ros_swi(struct ros_cpu *s, uint32_t number)`
  (`runtime/swi.c:233`), called on the engine's own thread under the
  personality lock. `struct ros_cpu` (`include/rosgd/cpu.h:59`) carries
  r0–r15, NZCV, Q, mode (`ROS_MODE_USR 0x10` / `ROS_MODE_SVC 0x13`),
  and the I bit — everything the conventions below need.
- **X bit** = bit 31 of the number. X-form error return: `s->v = 1`,
  `s->r[0]` = error block pointer (`ros_swi_fail`, `swi.c:96`) — the
  kernel's own two lines: V set into the CPSR destined by `SLVK`
  (`s/Kernel:429`, `MOVS pc, lr` at `s/Kernel:461`), R0 = error
  pointer validated at `s/Kernel:478-497`. Non-X error: a raise (§2.1).
- Registers out: whatever the handler left, per the kernel's epilogue.
  The engine copies `ros_cpu` back into `Regs()`/`Cpsr()` and returns
  from `CallSVC`; translated code resumes at the SVC's return address.

## 1a. Calls from ARM code to native code (#146, 4 October 2026)

- ARM code reaches native code three ways: a SWI (§1), a gateway address
  (`[ARM_GATE_LO, ARM_GATE_HI)`, design 25 §6a), and a branch to an
  address the runtime says is native (`arm_ops.native`: compiled code,
  a native entry, a C program's or module's code, native code BASIC
  assembled -- never an ARM range or the ARM application's memory).
- Both of the last two are fetched as `SVC &FFFFFE`; `on_svc` takes the
  address from R15 - 4, and the gate op (`box_gate`) calls it with
  `ros_call` and the ARM registers: r0-r15 (R14 the ARM caller's
  return), NZCVQ, the mode the bridge models. Back: the registers the
  native code left; the ARM code goes on at its R15 (for a gateway,
  always R14).
- An error raised in the native code is caught at the gate
  (`ROS_TRY`) and returned as `ARM_GATE_RAISED`, R0 its block: the
  bridge delivers it as a non-X SWI's error (§2.1), the raise's PC the
  return address. Nothing longjmps through the engine.
- The native code may call ARM code again (`ros_call` → `arm_task_call`):
  the next engine of the task, `ARM_DEPTH` (8) engines in all.
- A gate SVC whose address is no longer native invalidates itself and is
  fetched again as ARM; native code registered over an address
  (`ros_armrun_code_native`) invalidates what the engines had there.

## 2. The deliveries: how the OS enters the app's ARM code

Each kind names its kernel source; "mode" is the CPSR mode the engine
must install in the `ros_cpu` it injects; "claim" is the return
protocol the engine must honour when the handler returns.

### 2.1 Error handler (env #6)

`ErrHandler`, `s/Kernel:806-878`.

- Before entry the kernel fills `ErrBuf` with: word 0 = the raise's
  return PC, word 1 = error number, then the string (≤252 bytes)
  (`s/Kernel:837-845`); `TaskControl_ResetStacks` has flattened the
  SVC stack (`s/Kernel:849`).
- Entry: **USR mode, IRQ+FIQ enabled** (`s/Kernel:866`, `874` — via
  SPSR), R0 = **the handler's private word** (`s/Kernel:862-863`), R10–
  R12 = the caller's foreground values (`s/Kernel:857`), R1/R2 = not
  set by 5.31's code path (§7.1). The engine injects at `ErrHan` on a
  reset SVC stack with the buffer already filled.
- **No return protocol** — the handler is not expected to come back; a
  re-raise inside it re-enters flattened (`s/Kernel:816-830`).

### 2.2 Exit handler (env #11)

`SEXIT`, `s/Kernel:1357-1402`, whose ABI comment is quoted at
`s/Kernel:1390-1402`.

- Entry: **USR mode, IRQs enabled**, stacks reset (`s/Kernel:1379`),
  R0 = 0, R1 = caller's R1, R2 = return code, R12 = private word.
- No return; the default chain-ends into the next task's OS_Exit
  (`s/Middle:543-545`).

### 2.3 UpCall handler (env #16)

`DoAnUpCall`, `s/Middle:977-1010`.

- Entry: **SVC mode, IRQs enabled** (`s/Middle:988`), via UpCallV with
  the caller's R0–R3 (R0 = upcall reason), R12 = the vector node's
  workspace (`s/ArthurSWIs:345-346`).
- Claim = returning without passing on the chain; the R0 value returns
  to the OS_UpCall caller (`s/Middle:994-1009`). "R0 = 0 means
  claimed" is a caller-side convention only (§7.2).

### 2.4 Escape handler (env #9)

`Osbyte7C7D`, `s/PMF/osbyte:888-907`.

- Entry: the current mode (SVC when raised from OSBYTE), caller's IRQ
  state, R12 = private word; the escape flag is set before the call.
- **Claim = return with R12 = 1** (`s/PMF/osbyte:900-902`); anything
  else and the kernel queues an OS_SetCallBack.

### 2.5 Event handler (env #10)

`OSEVEN`/EventV, `s/NewIRQs:497-546`.

- Entry: **SVC mode, IRQs disabled** (`s/NewIRQs:492`), R0 = event
  number, R1–R2 = parameters, R12 = private word (`s/NewIRQs:536-537`).
- **Claim = return with R12 = 1** (`s/NewIRQs:539-546`); the claim
  suppresses the old-style callback the kernel would otherwise set.

### 2.6 Ticker events (OS_CallAfter / OS_CallEvery)

`TickOne` → `ProcessTickEventChain`, `s/TickEvents:151-161`.

- Entry: **SVC32 mode, IRQs disabled** (`s/TickEvents:151`), R12 = the
  value given to OS_CallAfter/Every, R0–R3 and R10–R12 corruptible
  (`s/TickEvents:128`). Plain return; no claim protocol; the chain
  continues.

### 2.7 Transient callbacks (OS_AddCallBack)

`process_callbacks_disableIRQ`, `s/Kernel:964-1005`; record layout
`{link, address, r12}` (`s/MoreSWIs:888-891`).

- Entry: **SVC mode, IRQs enabled** (`s/Kernel:986`), R12 = the
  workspace given; every other register is the interrupted context's.
- Plain return; the loop continues (`s/Kernel:1004`).

### 2.8 Old-style callback handler (env #7)

`Do_CallBack`, `s/Kernel:921-961`.

- Entry: R12 = **the 17-word register frame** (R0–R15 then PSR; word 0
  = return PC, word 16 = return PSR — `s/Kernel:945-960`), mode = the
  flattened USR return state. This is `ros_task.user_block`
  (`runtime/task.c:61-63`) — the mechanism already exists for native
  tasks; the engine supplies and reabsorbs the block.

### 2.9 Undefined instruction (env #1 / processor vector)

`UndPreVeneer`, `s/Kernel:377-378`; the kernel's own default path
`UNDEF`/`DumpyTheRegisters`, `s/Middle:835-853`, `640-646`.

- A claimant is entered **in UND32 mode, on the UND stack, with the
  faulting register set: R14_und = faulting address + 4, SPSR_und =
  faulting PSR**. Nothing else is set up; no instruction word is
  passed. The engine models UND mode for this delivery (its
  `ros_cpu.mode` gains the value; banked R14/SPSR live in the engine).
- **The 5.31 kernel contains no FPA chase** (grep finds only the
  `FPEAnchor` ReadSysInfo item, `s/Middle:1598`,
  `hdr/KernelWS:1641-1643`): the historic RISC OS 2/3 direct call into
  the module is gone. The FPEmulator is expected to claim this path
  itself and decode the instruction at `R14_und - 4`. §4 builds on
  exactly that.

## 3. The engine's obligations

- **Safe points.** Deliveries queue per owning task and drain at: every
  SWI return, the flatten path (below), and a counter on block exits —
  cheap, no trap (design 25 §4).
- **The flatten path.** A raise or delivery targeted at the engine's
  task unwinds to the engine loop's outer frame (the
  `ros_capp_deliver` pattern, `runtime/capp.c:1019`), installs the
  delivery's `ros_cpu` (registers, mode, IRQ state per §2), and resumes
  `Jit::Run` at the entry address (the unwind starts with
  `Jit::HaltExecution`). Translation state is per-block and
  disposable; the guest state *is* the `ros_cpu`.
- **Mode fidelity.** USR-mode entries (error, exit) must set the
  injected CPSR's mode bits and IRQ/FIQ as quoted; SVC/UND entries keep
  IRQ state per kind. The engine models exactly the modes the
  conventions use: USR, SVC, UND (for §2.9), and IRQ never (the runtime
  owns interrupt simulation). dynarmic itself is user-mode only, so the
  mode lives in `ros_cpu` and the engine swaps the banked R13/R14 (and
  SPSR) around each entry and return.
- **Stacks.** SVC-stack deliveries (2.3–2.7) run on the task's SVC
  stack slot at its fixed arena address, as compiled code does; UND
  (2.9) on a small engine-provided bank in the task's space.

## 3a. Raises the bridge maps (A2, 3 October 2026)

- The engine's `undefined`, `unpredictable`, `interpret` (patch 0003)
  and `decode-error` raises all become RISC OS's undefined-instruction
  error (&80000000, "Internal error: undefined instruction at &pc"), as
  RISC OS 5.30 on the A72 gives for every such form A2 tried (TEQP,
  MOVNV, `LDM ^`, `MOVS pc, lr`, LDM writeback with the base in the
  list). The FPA is the exception, routed to the native FPA first.
- A task starts with CPSR `&110` (USR, the A bit set), as 5.30's user
  mode reads it.

## 4. FPA: the emulator module, in the task

*Superseded 3 October 2026 (native first): the FPA is executed natively — dynarmic coprocessors
CP1/CP2 onto the task's `struct ros_fp`, the undefined-instruction
path as fallback — and the FPEmulator module is not emulated. The
conventions below remain the record of how 5.30 enters it.*

- The engine does not implement FPCP: no coprocessor is installed for
  CP1/CP2, so dynarmic raises `Exception::UndefinedInstruction` at an
  FPA instruction, delivered as §2.9 — UND32, `R14_und = addr + 4`, SPSR = the
  faulting PSR.
- The FPEmulator module's code and workspace live in the emulated
  task's own memory (container-private; never the shared RMA, never the
  native module chain). Its claim of the undefined path is a routing
  entry the loader records; its SWI chunk is routed into the container
  and answered by its emulated code over this same bridge.
- Its undefined handler decodes at `R14_und - 4` from task memory —
  ordinary arena reads, no special case in the engine.

## 5. Corpus: FPA / VFP / NEON / Thumb (the survey)

`tools/corpus_scan.py` is a word-level surveyor; its whole-disc run
over `v3-test-disc` (82 binaries) flags every category in every binary
— which is the tool's own caveat demonstrated: literal pools masquerade
as opcodes at word level. Read as a survey it says the shapes are
ubiquitous; as a verdict it says nothing yet. **The corpus verdict
comes from a decoder** run over real code sections — since 3 October,
dynarmic's own A32 disassembler (`interface/A32/disassembler.h`), in
A2; until then, the scope stands as decided: ARM state only, NEON and
VFP in, FPCP never (the FPE module answers it).

## 6. What the runtime already provides

`runtime/environment.c`'s `call_handler` family enters handlers with
`ros_cpu_enter` + R0–R2/R12 as *vector calls* — the SVC-mode,
caller-register contract of design 14. The container's deliveries use
the kernel's exact conventions above instead, which differ in mode and
claim protocol per kind; the engine, not the runtime, applies them.
`ros_user_return` (`runtime/callback.c:152-205`) already implements
§2.8's 17-word frame for native tasks.

## 7. Open, for RPCEmu verification

1. Error handler R1/R2 on entry — 5.31 never sets them; the PRM says
   R1 = error buffer. (`s/Kernel:811/831`)
2. UpCall "R0 = 0 means claimed" — not enforced in the kernel.
3. The FPEmulator's actual claim mechanism and register expectations
   (processor vector vs env handler; and whether any shipped FPE build
   relies on the old kernel chase) — the 5.31 kernel offers none.
4. Escape handler R0–R3 contents (sources show only R12 and the flag).
5. `ExitSWIHandler`'s expansion (defined outside this tree; presumed
   the SLVK tail, `s/Kernel:424-461`).
