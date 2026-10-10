# runtime/arch

The runtime is portable C over the arena (`include/rosgd/arena.h`) and builds for x86-64 and AArch64.
The machine code lives here in `.c.inc` files, which the shared source includes where it needs them. How arena C is built is described in `../../abi/`.

| File | Included by | Contents |
|---|---|---|
| `x86_64/capp.c.inc` | `runtime/capp.c` | the x32 gate: per-thread gate state, entries and exits, the static base in `%gs`, MXCSR and x87 control, trampolines |
| `x86_64/fault.h.inc`, `fault.c.inc`, `faultblock.c.inc` | `runtime/fault.c` | registers as a signal has them, the handler for `#DE`, `int3`, `#UD`, `#AC` and `#XM`, and the register block for the fault report |
| `aarch64/capp.c.inc` | `runtime/capp.c` | the A64X32 gate: entries and exits in A64, the static base in x18, FPCR, trampolines, instruction cache coherence, the code BASIC assembles for AArch64 |
| `aarch64/fault.h.inc`, `fault.c.inc`, `faultblock.c.inc` | `runtime/fault.c` | registers, PSTATE, ESR and FPSR, the ESR classifier, the handler, and the register block for the fault report |
| `aarch64/capp_jit.c.inc` | `runtime/capp.c` (hosted, `ROS_CAPP_A64_JIT` only) | for BBC BASIC V for Mac: runs the code BASIC assembles from copies in a `MAP_JIT` arena. No box compiles it |

The shared files reach the machine through names that each `capp.c.inc` defines (`FP_CTL_NOW`, `FP_LOAD`, `GATE_RET`, `gate_tramp`, `set_gs`, `SYNC_CODE_RANGE` and others) and each `fault.h.inc` defines (`fault_regs`, `FAULT_NATIVE_PC`, `FAULT_SP`, `FAULT_TO_PROGRAM`).

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
