# BBC BASIC VFP, hand translated

The ROM's BASIC is RISC OS 5.31's BBC BASIC VFP. It reaches the box in one of
two ways.

- `BASIC=lift`: `rosasm` translates the ROOL ObjAsm to C instruction by
  instruction. This is exact but slow.
- `BASIC=tier3` (the default in the Makefile): the interpreter written by
  hand in C from the same ObjAsm, and substituted into the lift function by
  function. This directory holds it.

A binary built with tier 3 has the symbols `basicvfp_hand_DISPAT` and
`basicvfp_hand_FACTOR`.

## Files

- `substitute.py` applies the manifest to the lift.
- `manifest.py` lists which labels, cases and functions are hand written.
- `units/` holds the hand written interpreter, one file per source unit.

No generated C is committed. `make build/gen/rom_basicvfp_t3.c` makes the
substituted source alone.

## What stays translated

The ROM image array and registration table, the core of the statement
dispatcher with four statement bodies (ASS, CLOSE, PROC, TWOSTMT), the
assembler family reached by `[` (the cross assemblers are in `../asmlib`),
the operator jump-table fragments, and VFPLib.

`../patch-basicasm.py` patches the lift before substitution. A hook inside a
body that a unit replaces is lost unless the hand body carries it, so
`substitute.py` refuses to write a twin that loses one.

## Source and tests

The development home is <https://github.com/albanread/basic-tier3>, which
also holds the status board and the gate rigs. To update, copy
`substitute.py`, `manifest.py` and `units/` from it. `make test` runs the
BASIC checks. A unit counts as landed when the seven-program suite and the 163
Rosetta programs match RISC OS 5.30's output byte for byte.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
