# rosasm -- where it stands (7 October 2026)

This replaces the review of 10 September ("urgent next steps"). Most of
what that review asked for has been done; what is left is listed below,
measured on the head build on the Mac.

| Measured | 10 Sep | 15 Sep | 7 Oct |
|---|---:|---:|---:|
| units in the BCM2835 build | 241 | 245 | 245 |
| assembling to an object | 212 | 219 | 228 (93.1%) |
| code bytes emitted | 240,596 | 243,092 | 402,452 in 282 areas |
| instructions emitted as zero words | 220 | 0 | 0 |
| failures | 29 | 26 | 17, of which 7 outside the build |
| tests | 319 | 333 | 431 |

`cargo build` and `cargo clippy --all-targets` are clean, and
`ruff check --select F,E9,B,PLE tools/` passes.

## What the 10 September review asked for, and what became of it

- **An object with wrong words in it must not exit 0.** Done: an
  unencodable instruction fails the unit; `--allow-unencodable` emits
  `UDF #n` instead and names each one.
- **Encoder errors must point at the source.** Done: every lowered
  instruction carries a `__ros<i>` label, and an encoder error is printed
  against the source line it came from.
- **The ten encoder rejections.** Gone, all of them; the two left are new
  (below). `TEQNEP` and its kind are refused as the 26-bit forms they are.
- **Driver and harness hygiene.** Done: `ROSASM_CLANG` names the encoder,
  temporary files carry a random suffix, and the doubled doc comment in
  `lower.rs` is gone.
- **The SPI assertion** and **an oracle run to completion** are still
  open (below).
- **From an object to a ROM.** Overtaken: the box's ROM is built from
  rosasm's C (`--emit c`) and ELF (`--elf`), not from linked AOF. An AOF
  object linked by the DDE's `link` and booted has still not been tried.

## Open

From `tools/codegen_sweep.py` on today's build:

- **Two encoder rejections.**
  - `SCSIDriver:3999-4000`: `LDR R4,DemandSlot+:INDEX:RamTxAdr` -- a
    label plus `:INDEX:` of a field; the expression reaches clang
    unfolded.
  - `Kernel s.Kernel:323`, `s.Utility:21`: an expression spanning two
    areas, `(EndOfKernelRW-RWBase)+EndOfKernelRO-...`, which ObjAsm
    accepts and rosasm does not yet.
- **`HAL_BCM2835 s.SPI:79`**: `. - 64 = HALDeviceSize` gets 0 against 64.
  The location counter is 64 short of where ObjAsm has it. HAL device
  blocks are laid out to the byte, so find the directive that moved `.`
  differently before trusting a HAL unit.
- **Expression syntax**: `GPIO:127` (an unterminated `:operator:`) and
  `Fused64:212` (an unclosed bracket).
- **`TML_HostFS:222`**: `ExitS without EntryS`.
- **`rlib s.poll:137`**: `UROM` undefined -- a build option the sweep does
  not pass.
- **Sources the build generates** that the sweep does not: `swioptions.s`
  (two units), `AppName.s`, `TokHelpSrc.s` (Sound0), `Macros.s`,
  `k_atomic_armk.s`, `Hdr:ZLib`, `OS:Hdr.Types`, `Fonts.Encodings.UTF8`.
  Seven of these units are not in the BCM2835 build at all.
- **The oracle.** The metric the design names -- units byte-identical to
  ObjAsm -- still needs `tools/aofdiff.py --all` run to completion against
  ObjAsm on the emulator. It was last measured as 4 of 18.

## Checks to run after a change

1. `cargo test --release` and `cargo clippy --release --all-targets`.
2. `tools/codegen_sweep.py <RiscOS> --jobs 1` before and after: the
   objects and messages should not move unless meant to.
3. In `../rosgd`, the units rosasm compiles for the box (`build/gen/rom_*.c`
   and the armrun images) rebuilt with the old and the new binary
   (`make ROSASM=... B=...`) and compared byte for byte.
