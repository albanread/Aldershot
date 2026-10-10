# Font Manager

The RISC OS Font Manager (`Video/Render/Fonts/FontManager`) as a native
module in C, with ROMFonts, which points `Font$Path` at the fonts in the
ROM. The code follows the 3.82 sources. It reports version 3.80, which is
what RISC OS 5.30 has. It uses only public calls: FileSwitch,
MessageTrans, OS_ReadVduVariables, OS_ReadUnsigned, and OS_SpriteOp, Draw
and SuperSample to make characters from outlines.

## Use

The module provides the `Font_` SWIs from &40080 and no star commands.
It also provides the VDU sequences VDU 23,26 (define a font), VDU 23,25
(thresholds and colours) and PLOT &D0 to &D7 (paint the string that
follows). At start it sets `Font$Path` if it is unset, and the `FCF`
(FontCache) file type with its run and load aliases.

| File | Contents |
| --- | --- |
| `fontmanager.c` | The module, the SWI table, the errors, the state SWIs. |
| `catalogue.c` | `Font$Path` scanning, ListFonts, the encodings list. |
| `encoding.c` | Encoding files and EnumerateCharacters. |
| `fonts.c` | FindFont, LoseFont, ReadDefn, LookupFont. |
| `files.c`, `metrics.c` | Font data files and metrics. |
| `arith.c` | Font_Arith and Font_BasFP. |
| `colours.c` | Anti-aliasing colours and the colour tables. |
| `pixels.c` | The character cache and the rasteriser. |
| `paint.c` | Font_Paint, Font_Caret and output to a buffer. |
| `scan.c` | ScanString, StringWidth, FindCaret, StringBBox. |
| `vduhooks.c` | The VDU 23 and PLOT hooks. |
| `romfonts.c` | ROMFonts. The fonts are in `resources/Fonts`. |

## Differences from RISC OS 5.30 and what is not done

- The font cache is not the original's. Fonts stay cached at usage 0
  until the handles run out, then the least recently claimed is reused.
  The sizes that CacheAddr reports mean nothing.
- The rasteriser's sprite and Draw path are in the RMA. There is no
  "Font cache full" error and the Hourglass is not turned on.
- EnumerateCharacters with R1 above &80000000 gives "Character code out
  of range". The original crashes.
- LookupFont on a master's handle gives "Undefined font handle".
- ReadInfo on a font with no data gives "Font data not found".
- CharBBox in OS units uses the outline box. The original uses the
  tighter bitmap box once the character has been painted.
- Not done: x90y45 files (4 bpp master bitmaps), magnified 4 bpp bitmaps,
  RAM scaled fonts, printer drivers, the font cache SWIs, `*FontList`,
  `*FontCat` and the `*Configure` keywords.
- Supremacy blending at 16 bpp uses the plain blend. Run length packed
  1 bpp bitmaps are decoded but not tested.
- Font_UnCacheFile does not uncache transformed data.
- With no CMOS, FontMax to FontMax5 start at the RISC OS 5 defaults.

Several quirks of the original are kept on purpose, for example the
one value cache in ConverttoOS and the kerning swap in right to left
text. They are marked in the code.

## Tests

`tests/vdu` compares registers and buffers with RISC OS 5.30, using the
streams `fonts28` (handles, definitions, encodings, scale factors,
ListFonts), `fontmetrics28`, `fontcol28`, `fontcol_16m`, `fontcol_64k`,
`fontbits28` (Font_MakeBitmap), `fontpaint28`, `fontpaint28b`,
`fontpaint28c`, `fontblend28`, `fontblend_16m`, `fontblend_64k`,
`fontbuf28` and `fontvdu28`. Painting is compared screen byte for screen
byte.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
