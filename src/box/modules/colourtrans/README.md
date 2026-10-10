# ColourTrans

A native module that follows ROOL's ColourTrans (`Video/Render/Colours`,
version 1.97). It finds colour numbers, GCOLs, dither patterns and translation
tables for physical colours. As in the original, every SWI goes through ColourV
with R8 set to the SWI's offset, so a claimant can take any of them. The module
uses only public calls: OS_ReadModeVariable, PaletteV, OS_SpriteOp,
OS_SetColour and the Font Manager's SWIs.

The SWI chunk is &40740. The SWIs are the original's.

| File | What it does |
|---|---|
| `colourtrans.c` | The module, ColourV, the services, the colour SWIs, ReadPalette and WritePalette |
| `match.c` | Palettes, matching, GCOL and colour number conversion, and the colour cache |
| `dither.c` | The SetGCOL dither patterns |
| `tables.c` | SelectTable, SelectGCOLTable, GenerateTable and the 32K tables |
| `models.c` | Calibration, the colour models and the desktop save SWIs |
| `fontcol.c` | ReturnFontColours and SetFontColours |

## Differences from RISC OS 5.30

The original's results are kept, quirks included. A few cases depend on the
original's own code bytes, on uninitialised memory, or on a crash. These give a
defined result here instead:

- An 8 bpp full-palette mode with the default palette uses the standard 256
  colours after the first 32 entries.
- Unset entries in a 32K table to an 8 bpp destination are 0.
- HSVToRGB with a hue outside 0 to 720 uses sector 0.
- WriteCalibrationToFile stops the second line after 16 data words.
- Colour number conversion of a value of 256 or more wraps the index.

Not done yet: the star commands (`ColourTransMapSize`, `ColourTransMap`,
`ColourTransLoadings`) and the service for desktop saving. Font colours are
written but untested. The file-writing SWIs are untested against RISC OS 5.30.

## Tests

`tests/vdu/streams.py` runs the streams `ct28`, `ct_16m`, `ct_c256` and
`ct_64k`. They run the same operations in MODE 28, at 16M colours, at 256 colours
and at 64K colours. Every SWI's registers and every table byte are compared with
RISC OS 5.30, along with the screen.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
