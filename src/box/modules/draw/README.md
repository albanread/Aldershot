# Draw

A native module that follows ROOL's Draw module (`Video/Render/DrawMod`,
Drawing Module 1.22). It fills and strokes paths (moves, lines, Beziers, closes
and gaps) with any winding rule and fill style, thin or thick, with joins, caps
and dashes, through a matrix. It also flattens, transforms, counts and boxes
paths. SWI chunk &40700, with the original's SWIs.

As in the original, every SWI goes through DrawV and becomes a
`Draw_ProcessPath` call that goes through DrawV again, so a claimant sees them
as it does on RISC OS. The module uses only public calls: OS_ReadVduVariables,
OS_ChangedBox, OS_RemoveCursors, OS_RestoreCursors and the VDU's exported HLine.

| File | What it does |
|---|---|
| `draw.c` | The module, DrawV, the SWIs and the ProcessPath checks |
| `process.c` | The stages: transform, flatten, split, thicken, thin strokes and dashes |
| `output.c` | The outputs: a path in place or to a buffer, a count, a bounding box, and fills |
| `arith.c` | The original's double precision routines and heap sort |

The code is the original's, transliterated, including its quirks. Examples are
the transform stage's cached last point and the behaviour of
`Draw_FillClipped` at the ends of a clipping span.

## Differences from RISC OS 5.30

- Errors are the UK texts, built in, not looked up in a Messages file.
- A cap or join that fails while a move or a close with a gap is thickened
  returns its error. The original returns the new point's X as the error pointer.
- `Draw_FillClipped` with an error in the main path returns that error. The
  original returns R0 with V set.
- The workspace is allocated with malloc, so "No room" cannot happen.
- Floating point forms give "not in this version of Draw", as in the original.

## Tests

`tests/vdu/streams.py` has the streams `draw28a` to `draw28e`. They compare
every screen pixel, register and buffer byte with RISC OS 5.30 on the farm. They
cover every fill style, Beziers, matrices, thin and thick strokes, dashes, clipped
fills, buffer, count and bounding box outputs, and the errors. The harness takes
SpecialFX out for these streams, because it replaces Draw with GDraw.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
