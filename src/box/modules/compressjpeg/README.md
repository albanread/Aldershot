# CompressJPEG

A native module that follows ROOL's CompressJPEG (`Video/Render/JCompMod` 0.08)
and uses libjpeg-turbo 3.2.0 (`deps/build-imagelibs.sh`). Paint's JPEG export
uses it (#99). RISC OS 5.30 loads it from `!System.Modules` as `JCompMod`. The
box has it in the ROM. The interface is PRM 5a-617 to 5a-623, and the original's
`c/jcompmod` for Comment.

## SWIs (base &4A500)

| SWI | In and out |
|---|---|
| `CompressJPEG_Start` | R0 buffer, R1 its size, R2 block (width, height, quality, components, x and y dpi), R3 workspace or 0, R4 its size. Out R0 the tag |
| `CompressJPEG_WriteLine` | R0 tag, R1 row of RGB triples or grey bytes |
| `CompressJPEG_Finish` | R0 tag. Out R0 the length of the JPEG |
| `CompressJPEG_Comment` | R0 tag, R1 bit 0 set for a string at R2, else data at R2 with length R3. Before the first row |

Only one compression is active at a time. Starting another abandons the first.
Every libjpeg failure is `&8183C4 Not enough memory`. A comment of 65000 bytes
or more is `&8183C5 String too long`.

## Differences from RISC OS 5.30

- The markers match 5.30 byte for byte. The entropy-coded data comes from
  libjpeg-turbo, which rounds a little differently from the IJG's release 5, so
  the image files differ. Decoded, they are within a mean of 2.
- Where 5.30 hangs, this module returns "Not enough memory". These cases are
  Finish with too few rows, a comment after the rows, and rows past the height.
- libjpeg-turbo needs more workspace than release 5. The module takes what a
  workspace of the PRM's size lacks from the RMA.

## Tests

- `boot/selftest_compressjpeg.c`, in both boxes.
- `tests/desktop/compress/jpeg.bas` and `jpegerr.bas`, against RISC OS 5.30.
- `tests/deskprobe/paint.py` saves a JPEG from Paint.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
