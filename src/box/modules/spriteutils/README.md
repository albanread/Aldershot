# SpriteUtils

A native C version of the RISC OS 5 SpriteUtils module (`Video/Render/SpriteUtil`, 1.13). It provides the * commands for the system sprite area. Each command is a call to OS_SpriteOp on that area, so errors are OS_SpriteOp's and FileSwitch's, passed on unchanged.

| Command | OS_SpriteOp |
| --- | --- |
| `*SChoose <name>` | 24 SelectSprite |
| `*SGet <name>` | 14 GetSprite, no palette |
| `*SFlipX <name>`, `*SFlipY <name>` | 33 and 47 |
| `*SDelete <name> [<name>]` | 25 DeleteSprite, for each name |
| `*SList` | 8 ReadAreaCB, then 13 ReturnName |
| `*SLoad <file>`, `*SMerge <file>`, `*SSave <file>` | 10, 11, 12 |
| `*SNew` | 9 ClearSprites |
| `*SInfo` | 8 ReadAreaCB, prints size and use |
| `*SRename <old> <new>`, `*SCopy <name> <new>` | 26, 27 |
| `*ScreenSave <file>`, `*ScreenLoad <file>` | 2, 3, with the palette |

Text comes from `Resources:$.Resources.SpriteUtil.Messages`. With no sprite area, `*SInfo` and `*SList` print "No system sprites memory". With no sprites, `*SList` prints "No system sprites defined".

Differences from RISC OS 5.30: the help string carries its own date (the version, 1.13, is 5.30's). The help and syntax texts are in the module, where 5.30 looks them up as tokens. `*Help` prints the same.

Tests: `tests/desktop/sprutils` (the sprites, modules and help probes, compared with 5.30 on the farm) and `boot/selftest_sprutils.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
