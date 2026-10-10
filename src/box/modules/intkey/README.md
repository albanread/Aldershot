# InternationalKeyboard

InternationalKeyboard is the key handler: the code that decides which
character a key makes. It is a native C module (`intkey.c`) transliterated
from `Internat/IntKey` (`Source/IntKeyBody`) in the ROOL sources. The
kernel owns KeyV and asks the handler about each key. The kernel's side is
`runtime/keyboard.c`.

## Use

There are no commands and no SWIs. It answers three services:

| Service | Answer |
| --- | --- |
| Service_KeyHandler (&44) | For keyboard ID 1, 2, 3, 4 or &FF it installs its handler with the kernel and claims the call. For any other ID it puts the old one back. |
| Service_Reset (&27) | Sets itself up again. |
| Service_International (&43), R2 = 6 | Sets itself up again for the new keyboard number (OS_Byte 71). |

The handler counts Shift, Ctrl and Alt, handles the mouse buttons and
Break, and follows Num Lock, Caps Lock and Scroll Lock. Alt with keypad
digits makes a character by its number. Other keys go through the layout's
Unicode table, with dead accents, and come out in the current alphabet.
The International module supplies the alphabet's table (Service_International
reason 8).

The layout tables are generated at build time. `tools/keylayout.py` runs
IntKey's own keygen on `layout/UK` and turns the result into C in the build
tree.

## Differences from RISC OS 5.30 and what is not done

- Only the UK layout is carried. Every keyboard number types the UK keys,
  in the alphabet that is selected.
- Wide (16 bit) key numbers and FN tables are not supported. The generator
  refuses an FN table.

## Tests

- The `keyboard` case in `tests/vdu/streams.py` compares KeyV, INKEY and
  the OS_Byte variables with RISC OS 5.30.
- The `keyboard` probe in `tests/desktop/international` types the same keys
  in eight alphabets.
- `boot/selftest_international.c` tests Latin2.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
