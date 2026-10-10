# ColourPicker

This is RISC OS's own ColourPicker (Acorn's C, Apache 2.0: `c/`, `support/`
and `Resources/`), compiled against `oslcr`, the OSLib compatible library
in this repository, and the ROM C library. It is an x32 module, the first
C module in the ROM.

## Use

`make picker` builds `build/capps/picker/Picker,ffa`. It has one cmhg
description (`PickerHdr.cmhg`), the module's C, and `c/veneer.c`. The
veneer replaces `s/veneer.s`, which the Apache drop does not carry. It is
written for the box's entry contract, with the Wimp filters as nested x32
entries. The module is linked with `roscc link --module --clib` and built
with `-DROM`, so its resources are the ROM's (`resources/Resources/Picker`).

The ROM carries the image (`tools/mkromx32.py`). `rom.c` copies it to the
RMA, applies its fix-ups and starts it from its module header. The SWIs
and the dialogue are RISC OS's, for example `ColourPicker_OpenDialogue`
with the RGB, CMYK and HSV models.

## Differences from RISC OS 5.30

- `support/c/relocate.c` read the ARM C library's relocation words at the
  SVC stack base, which is a guard page here. The box's `relocate_begin`
  and `relocate_end` do nothing, because the picker's models are in the
  picker. The original is kept beside it.
- `support/c/callback.c` read its variadic keys from the stack as an array.
  x32 and A64X32 pass them in registers, so it uses `<stdarg.h>`.
- The Wimp filters in `c/veneer.c` call `main_pre_filter` and
  `main_post_filter` as `s/veneer.s` did, and keep the Filter Manager's
  return conventions.

These were the fixes for #21 (a deadlock in the heap mutex at the first
`malloc`) and #63 (the dialogue did not open or redraw).

## Not done

No per-function register test of the module has been run in the box
against RISC OS 5.30. Two constants in `oslcr/h/os.h` (the PRM's global
errors) are flagged for it.

## Tests

`boot/selftest_capps.c` and `tests/deskprobe/paint.py` open the dialogue.

Acorn's ColourPicker source is under Apache 2.0.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
