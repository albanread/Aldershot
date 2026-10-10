# International

International is the RISC OS International module as a native module in C
(`international.c`). It holds the names and numbers of countries and
alphabets, and draws the system font's characters for an alphabet. It
follows `Internat/Inter` 1.71 from the ROOL sources, which matches the 1.70
(06 Feb 2021) in the RISC OS 5.30 ROM. The tables are compiled from that
source by `tools/mkinternational.py`. The build has NewAccents and DoUTF8
on, and DoBfont off, so there is no Bfont or Cyrillic2 alphabet.

## Use

Everything is through Service_International (&43), with the reason in R2.

| R2 | Does |
| --- | --- |
| 0, 1 | country or alphabet name to number |
| 2, 3 | country or alphabet number to name |
| 4 | a country's alphabet |
| 5 | define characters in the system font, for an alphabet |
| 7 | define one character from a UCS code |
| 8 | give the UCS table of an alphabet |
| 9 | ISO 3166-1 alpha-2 code of a country |

Reason 6 (the keyboard changed) is passed on. The system font is changed
with VDU 23, so a VDU stream or a task window sees the change.

Commands: `*Alphabet`, `*Alphabets`, `*Country`, `*Countries`,
`*Keyboard`, `*Configure Country` and `*Status Country`. Errors are &640
"Unknown alphabet", &641 "Unknown country" and &642 "Unknown keyboard".
There are no SWIs.

The kernel's OS_Byte 70 and 71 (`runtime/keyboard.c`) ask this module for
the alphabet of a country, and OS_Byte 20 and 25 offer it reason 5 first.
Its clients are the Territory Manager, FontManager, InternationalKeyboard
(`modules/intkey`), the Wimp and !Chars.

## Differences from RISC OS 5.30

- It starts before InternationalKeyboard, in the place the Pi ROM gives it.
- The UCS tables are copied into the RMA once, so their address stays good
  if the module is killed and restarted.
- OS_Byte 25 returns &FFFFFFFF in R1 when it copies the kernel's own font,
  because the font is in no ROM here.
- Only the UK keyboard layout exists (see `modules/intkey`). Selecting
  another keyboard changes the alphabet and the keyboard number only.

## Tests

- `tests/desktop/international` compares the module with RISC OS 5.30
  (`compare.py`, with `--farm` or `--guest`).
- `boot/selftest_international.c` checks what must always hold.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
