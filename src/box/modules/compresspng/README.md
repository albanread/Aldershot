# CompressPNG

A native module that follows ROOL's CompressPNG (`Video/Render/CompressPNG`
0.07, Apache 2.0) and uses libpng 1.6.50 and zlib 1.3.2
(`deps/build-imagelibs.sh`). Paint's PNG export is its main user (#99). RISC OS
5.30 loads it from `!System.Modules` as `PCompMod`. The box has it in the ROM.
The interface is the original's `doc/SWIs`.

## SWIs (base &59E00)

| SWI | In and out |
|---|---|
| `CompressPNG_Start` | R0 output buffer, file name (flags bit 0) or 0 for the size alone, R1 buffer size, R2 block (width, height, x and y dpi, flags, parameters). Out R0 the tag |
| `CompressPNG_Comment` | R0 tag, R1 key, R2 value (a tEXt chunk) |
| `CompressPNG_WriteLine` | R0 tag, R1 row |
| `CompressPNG_Finish` | R0 tag. Out R0 and R1 are the buffer and its length, or 0 and the size |

Palettes are lists of words with red in the low byte, as Paint and 5.30 use
them. A gamma is a double with the high word first.

Each Start makes a dynamic area named `CompressPNG`, as the original does.
Finish removes it, so the tag is invalid afterwards. A task that ends without
finishing loses its areas. Errors are from `ErrorBase_CompressPNG` (&821600).
The module checks that ZLib is loaded, as the original does.

## Differences from RISC OS 5.30

None are known. The PNGs it makes are byte for byte the same as 5.30's for the
28 test images. Memory that a program names and that is not there gives
`&80000002 Internal error: abort on data transfer`.

## Tests

- `boot/selftest_compresspng.c`, in both boxes.
- `tests/desktop/compress/png.bas`, against RISC OS 5.30.
- `tests/deskprobe/paint.py` saves a PNG from Paint.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
