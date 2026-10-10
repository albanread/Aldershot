# dynarmic, as the ARM container takes it

The ARM container's engine is
dynarmic, built by `deps/build-dynarmic.sh` from a pinned commit with the
patches in `patches/`. The source is not copied into this repository:
the script clones the pinned commit and the externals it pins as git
submodules into `.cache/dynarmic-src`, as `deps/` fetches every other
third-party library the box carries.

**Our mirrors.** Since 4 October 2026 the script fetches from our own
private mirrors on GitHub, over SSH, so the build does not depend on
upstream staying:
- `albanread/dynarmic`, and `albanread/dynarmic-<name>` for each external
  it builds: fmt, mcl, oaknut, robin-map, xbyak, zydis, zycore. Each is
  a full mirror (branches and tags) holding the pinned commits.
- The Boost archive is the release `boost-1.89.0` on `albanread/dynarmic`.
- `DYNARMIC_UPSTREAM=1` takes everything from upstream instead.

A clean build from the mirrors gave libraries byte for byte the same as
the upstream ones. A local copy is in `third_party/dynarmic` and
`third_party/Boost` (`third_party/CATALOGUE.md`). This file is the record of what is
taken, under which licence, and what we change.

## The pin

| Component | Where | Commit | Licence |
|---|---|---|---|
| dynarmic | `github.com/azahar-emu/dynarmic` (Azahar's fork; the original, merryhime's, archived March 2024) | `a46601580d5512d324104f985b5f0209dc980ddc` (26 Sep 2026) | 0BSD |
| mcl | `externals/mcl` (submodule) | `5fc4beaf33` | MIT |
| oaknut (the AArch64 assembler) | `externals/oaknut` | `94c726ce03` | MIT |
| xbyak (the x86-64 assembler) | `externals/xbyak` | `8c0098b69f` | BSD-3-Clause |
| robin-map | `externals/robin-map` | `054ec5ad67` | MIT |
| fmt | `externals/fmt` | `7b273fbb54` | MIT (with fmt's binary exception) |
| zydis, zycore (x86-64 disassembly, debug output only) | `externals/zydis`, `externals/zycore` | `bffbb610cf`, `0b2432ced0` | MIT |
| Boost, headers only (`icl`, `variant`) | `archives.boost.io` 1.89.0, SHA-256 checked | `boost_1_89_0.tar.bz2` | BSL-1.0 |

Not built: biscuit (RISC-V's back end), Catch2 (dynarmic's tests; A2
may build them), LLVM (disassembly), Unicorn (fuzzing). Each licence was
read from the component's own file on 3 October 2026. Nothing GPL or
LGPL is admitted (design 25 NA-d).

**Notices.** MIT and BSD-3-Clause ask for their notices with binary
copies: an end-user box (`/init`, which links the engine) ships them in
its third-party notices with OpenSSL's, curl's and the rest. 0BSD and
fmt's exception ask for nothing.

## The build

- One host back end per build: AArch64 for `aarch64-linux-musl` (the
  Apple Silicon box) and for the Studio's hosted test; x86-64 for
  `x86_64-linux-musl` (the Intel box). The A32 front end only
  (`DYNARMIC_FRONTENDS=A32`).
- Box builds: `zig c++`, static, `-fPIE` (/init is a static PIE),
  `-DDYNARMIC_EXTERNAL_SIGNALS` (patch 0001). Linked into `/init` with
  zig's libc++ (`-lc++`).
- Output: `lib/libarmrun-engine.a` (dynarmic, fmt, mcl, and Zydis and
  Zycore for x86-64) and `include/` (dynarmic's headers and mcl's) under
  `.cache/[aarch64/]dynarmic-musl` or `.cache/dynarmic-host`.

## Our patches

Seven, applied in order to a clean checkout of the pin; the build's stamp holds
the pin and a hash of the patches, so a change to either rebuilds from
clean. Each changed place in the source carries a `ROSGD
(runtime/armrun/patches/000N)` comment.

### 0001 — the runtime owns the signals

`backend/exception_handler_posix.cpp`. Upstream, the first JIT created
installs a SIGSEGV handler (SIGBUS too on macOS) that recovers fastmem
faults in translated code and chains to the previous handler, and calls
`sigaltstack` on the creating thread with a 2 MB stack of its own. In
the box that would replace an ARM task thread's signal stack and put
dynarmic in front of the runtime's handler (`runtime/fault.c`), which
owns those signals for the whole process (design 25 §6). With
`DYNARMIC_EXTERNAL_SIGNALS` nothing is installed; `extern "C"
dynarmic_handle_fault(sig, info, context)` answers the same question —
1 when the fault was in JIT code and the context now points at its slow
path, 0 otherwise — and the runtime's handler asks it first
(`ros_fault_jit`, `include/rosgd/fault.h`; `armrun_handle_fault`).

### 0002 — invalidate every block a range covers

`backend/block_range_information.{h,cpp}` and the declarations that
name its range type (both back ends, A32 and A64). Found by A1's
invalidation test, and the fault azahar-emu/dynarmic#15 reports
(closed unmerged).

Upstream keeps each translated block's address range in a
`boost::icl::interval_map` and finds the blocks a range covers with
`equal_range`. ICL's interval containers sit on `std::map` with overlap
as key equivalence, and libc++ (measured with the Studio's Apple clang;
the box's zig libc++ is the self-test's to show) searches a unique-key map — `equal_range`, `lower_bound`, `upper_bound` — by
stopping at the first equivalent node it meets. So a range over several
blocks found one of them, whichever the tree search met first:
invalidating `&A000–&A03F` over four blocks retranslated only the one
at `&A010`. ICL's own `interval_set::add` merges through the same
searches, and under libc++ leaves overlapping pieces where the union
should be one (measured: adding `&A008–&A047` to three separate ranges
left three overlapping pieces, not `&A000–&A04F`).

The patch replaces the block index with a `std::multimap` from each
block's first address to its last, searched by point from (range start −
longest block) — no ordering can make that ambiguous — and erases the
blocks it invalidates (upstream left them, a `TODO: EFFICIENCY`). The
pending ranges become `Backend::RangeSet`, a vector kept as given, in
place of `icl::interval_set`. Boost remains for `discrete_interval`
values and `boost::variant`, neither of which searches a map.

### 0003 — raise what upstream would interpret

`frontend/A32/translate/impl/a32_translate_impl.cpp`, `interface/A32/config.h`.
Found by A2's probe of the corpus. Upstream translates a few instructions
as an `Interpret` terminal — LDM/STM with the user-bank `^`, LDM `^` with
pc (the 26-bit flag-restoring return), CPS, RFE, SRS — and dynarmic has no
interpreter: the AArch64 back end asserts ("Interpret should never be
emitted"), aborting the process, and the x86-64 back end calls
`InterpreterFallback`. They are raised instead, as a new last
`Exception::InterpretRequired` (`ARMRUN_INTERPRET`), the same on both
back ends. RISC OS 5.30 on the A72 gives an undefined instruction for
every one of them in user mode (A2's idioms), so the bridge raises that.

### 0004 — a coprocessor instruction no coprocessor takes is undefined

`frontend/A32/translate/impl/coprocessor.cpp`, `frontend/A32/translate/a32_translate.h`,
the two `Translate` calls. Found by A2's probe: with no coprocessor
installed for CP1/CP2, every FPA instruction asserted in both back ends
("Should raise coproc exception here"). The translator now has the
configured coprocessors (`TranslationOptions::coprocessors`) and asks the
one the instruction names whether it takes it (its `Compile*` hook, the
question the back end would ask); if none does, the instruction raises
`UndefinedInstruction` when its condition passes, as the hardware does.
FPA instructions reach the bridge this way until the native FPA
coprocessors are installed (design 25 §6).

### 0005 — single-stepping an instruction that raised

`frontend/A32/translate/translate_arm.cpp`. Found by A2's probe: stepping
an NV-condition instruction (the pre-ARMv5 "never" encodings, now
unallocated) raised an exception, which sets the block's terminal, and
then single-step mode set a second one ("Terminal has already been set").
The step terminal is set only when none is. Normal runs were not
affected; the differential harness and debugging step.

### 0006 — the memory-abort check reads the halt word as 32 bits

`backend/arm64/emit_arm64_a32.cpp`. Found by A3: with
`check_halt_on_memory_access` on (armrun.cpp turns it on, so a data
abort stops the guest at the access, R15 that instruction), the AArch64
back end's check loaded the 32-bit, 4-aligned halt word with a 64-bit
`LDAR`, which needs natural alignment and faulted inside JIT code
("Segfault wasn't at a fastmem patch location"). It is a 32-bit `LDAR`
now, as every other load of the word is. The check is emitted only on
the paths a fastmem fault falls back to, so ordinary accesses do not pay
for it; on x86-64 the option also turns off the get/set elimination
pass (A6 measures that).

### 0007 — MSR's control field raises

`frontend/A32/translate/impl/status_register_access.cpp`. Found by A4:
RISC OS 5.30's SharedCLibrary, run emulated, changes mode with `MSR
CPSR_c`, which upstream (user mode only) drops silently, so the bridge
never saw the library go back to user mode. An MSR that writes the control
field now raises `InterpretRequired`; the bridge executes it -- the mode,
banked R13/R14, I and F in a privileged mode; the flags only in user mode,
as the hardware does (`bridge.c`, `privileged`).

### Known differences (not patched)

- **A load whose value is never used is dropped** by the engine's dead
  code elimination, so it cannot fault: a program that probes memory
  with `LDR` and ignores the value gets no data abort where RISC OS
  would give one (A3, `tests/armrun/hello.s`'s abort case uses its
  value for this reason).
- **An FPA trap is raised at the end of its block** (A4). A coprocessor
  callback has no PC and no way to stop the block, so a trapped FP
  exception (an FPSR trap enable: C programs enable invalid operation,
  division by zero and overflow) is delivered when the block ends; the
  bridge ignores the block's later SWIs and exceptions meanwhile, but its
  stores still happen, and the error's PC is the block's.

**Every distinct 32-bit word in the corpus** — 767,537, code and data
alike — now translates as one instruction on the AArch64 back end without
an assertion (A2).

## x18 on AArch64

A64X32 code keeps its static base in x18 (`abi/a64x32/README.md`), and
the gate sets it on each entry into arena code. The engine is runtime
code: its translated blocks never allocate x16–x18 (dynarmic's
`backend/arm64/abi.h`, `GPR_ORDER`), and its C++ may use x18 as any
runtime C may, under the gate's existing save and restore. So no x18
case is needed here; A4's first SWI from emulated code into an A64X32
module exercises the path for real.
