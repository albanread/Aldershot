# SpriteExtend

A native version of RISC OS 5's SpriteExtend (`Video/Render/SprExtend`, 1.86). It claims SpriteV and uses only the kernel's public calls (OS_ReadVduVariables, OS_ReadModeVariable and OS_SpriteOp). The original builds ARM code for each plot (`c/PutScaled`, `c/asmcore`), which the BOX cannot run. Here the effect of that code is written as a function of each destination pixel.

## What it provides

| Reason or SWI | Source file |
| --- | --- |
| 17 CheckSpriteArea, 35 AppendSprite, 37 CreateRemovePalette, 57 and 58 Insert/DeleteRows/Columns | `spriteextend.c` |
| 36 SetPointerShape | `spriteextend.c` (plots through 52, then OS_Word 21) |
| 38 CreateRemoveAlpha | `spriteextend.c` (masks and alpha channels, each converted to any other, in place) |
| 50, 51, 52, 65 (mask, character, sprite and tile plots, scaled) | `scaled.c` |
| 55, 56 (mask and sprite plots, transformed) | `scaled.c` |
| 53 PutSpriteGreyScaled | refused, as RISC OS 5 refuses it |
| JPEG_Info, FileInfo, PlotScaled, PlotFileScaled, PlotTransformed, PlotFileTransformed, PDriverIntercept (&49980 to &49986) | `jpeg.c` |

## JPEG

Decoding uses libjpeg-turbo (the library in `/init` that CompressJPEG and ChangeFSI use) with the fast integer IDCT, as SpriteExtend does. The upsampling and colour conversion are the original's. Progressive and arithmetic-coded JPEGs plot. Lossless, 12-bit and unusual sampling factors are refused with "JPEG format is not supported". The last image decoded is kept for the next plot of the same bytes.

The original's output paths are kept, including its 16 bpp and 8 bpp dithers, error diffusion, the 1:6 "postage stamp" DC-only decode, and its quirks. JPEG_PlotTransformed only scales, as in 5.30. A rotation or negative scale gives "Transformed JPEG plotting is not supported".

## Differences from RISC OS 5.30 and not done

- Blending onto 256 colours or fewer needs BlendTable and InverseTable. It gives an error until they exist.
- JPEG colour mapping (R5 bit 3) is the sprite path's, and is untested against 5.30.
- JPEG_PDriverIntercept with bit 0 set sends plots to PDriver_JPEGSWI, which the BOX has no printer driver to take.
- Copied from the farm: a true-colour sprite on 256 colours or fewer with no table plots nothing and gives no error. A 16 bpp sprite drawn translucent, masked and dithered onto 16 bpp plots nothing.

## Tests

`tests/vdu` (`sprext28`, `sprext_16m`, `sprext_64k`, `trans28`, `trans_16m`, `alpha28`, `setpointer28`, `vdu528`, `vdu5_16m`) plot on the farm and in the BOX and compare the results byte for byte. `tests/desktop/jpegplot` plots 15 JPEGs into sprites of six depths twenty ways. 1710 of 1755 sprites match 5.30 pixel for pixel. The other 45 are two cases that 5.30 does not draw reproducibly. `boot/selftest_jpegplot.c` checks the rest. `tests/deskprobe/jpegview.py` and `tests/deskprobe/changefsi.py` test the desktop uses.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
