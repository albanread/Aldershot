# SuperSample

A native version of RISC OS 5's SuperSample (`Video/Render/Super`, 0.16). It reduces a 1 bpp image of 4n+3 rows to a 4 bpp image of n rows. The Font Manager uses it for anti-aliased characters.

| SWI | Behaviour |
| --- | --- |
| `Super_Sample90` | Weights a 7 x 7 grid (1 2 3 4 3 2 1 each way). |
| `Super_Sample45` | The same for half-height pixels, over a 9 x 7 grid. |

The code is a transliteration of the ROOL assembler, including its lookup tables (Matrix1 and Matrix2). Two quirks of Sample45 are kept as they are in the original. Its eighth pixel is centred on bit 31 of its word, and its seventh loses the top column of the window.

The output may overwrite the input. R0 returns the last pixel made, or 3 for an image of 3 rows, which makes nothing.

Tests: `tests/vdu`, stream `draw28e`. It compares both SWIs, to another buffer and in place, over random images and their errors, against RISC OS 5.30 on the farm.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
