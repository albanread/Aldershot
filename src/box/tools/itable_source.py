#!/usr/bin/env python3
"""itable_source.py <ITable's s/ITable> <out> -- ITable's source for the ROM.

Two changes to ITable 0.18's source, in a copy in the build tree (it is
RISC OS's source, never put into this repository):

- It imports one routine from AsmUtils, usermode_donothing, which drops
  to user mode and back so that callbacks run during its long
  calculation; rosasm's C back end has no linker for it.  ROSGD's runtime
  collects callbacks itself, so here it is a routine of the unit's own
  that returns.
- claim_vectors returns when PaletteV is *not* yet claimed ("EXIT EQ"
  after testing f_PaletteV), so as written the table is never made again
  when the palette changes.  RISC OS 5.30's ITable, 0.18 too, makes it
  again after a VDU 19 (the farm: tests/vdu itable28), as if it claimed
  PaletteV -- so this copy returns only when it is already claimed.
"""
import sys

IMPORT = "        IMPORT  usermode_donothing\n"
CLAIM = """        TST     R3, # f_PaletteV                ; is paletteV claimed currently
        EXIT    EQ
"""
STUB = """; ROSGD: AsmUtils' usermode_donothing drops to user mode and back so that
; callbacks run; ROSGD's runtime collects callbacks itself
usermode_donothing
        MOV     pc, lr

"""


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding="latin-1").read()
    if src.count(IMPORT) != 1:
        sys.exit("itable_source.py: the import is not where it was")
    src = src.replace(IMPORT, "")
    if src.count(CLAIM) != 1:
        sys.exit("itable_source.py: claim_vectors is not as it was")
    src = src.replace(CLAIM, CLAIM.replace("EXIT    EQ", "EXIT    NE"))
    end = src.rindex("        END")
    open(sys.argv[2], "w", encoding="latin-1").write(src[:end] + STUB + src[end:])


if __name__ == "__main__":
    main()
