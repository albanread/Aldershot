"""Where things are, on whichever machine this is.

rosasm is written on two machines, so none of these is a constant. What is
built in this tree is found from this file's own location, because that is
where it will always be, whatever the tree is called or which drive it sits
on. What lives outside the tree -- the RISC OS sources, the emulator, the
ROMs -- sits beside the tree in a development root, so that too is found
from this file rather than written down. Every one of them can be named by
an environment variable where a machine puts it somewhere else.

The encoder reads the same `ROSASM_CLANG` the assembler itself reads, so one
variable configures a whole build rather than each tool separately.
"""

import os

# This file is tools/paths.py, so the tree is one directory up.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What an executable is called here.
EXE = ".exe" if os.name == "nt" else ""


def _env(name, default):
    """An environment variable, where it is set and not empty."""
    value = os.environ.get(name)
    return value if value else default


def binary(name):
    """A rosasm binary.

    Built in this tree unless a variable of the same name says otherwise, so
    a sweep can measure one build while the emulator harness is executing
    another. The suffix is the platform's, which is why this is not a plain
    string substitution: `rosasm.exe` -> `aofdump.exe` is fine on Windows and
    silently wrong anywhere else.
    """
    return _env(name.upper(), os.path.join(ROOT, "target", "release", name + EXE))


ROSASM = binary("rosasm")
AOFDUMP = binary("aofdump")
ROSLIST = binary("roslist")
DIFFTEST = binary("difftest")

# The encoder. On Windows clang is not on PATH by convention, so name the
# installer's path; everywhere else the bare name lets PATH answer.
CLANG = _env(
    "ROSASM_CLANG",
    r"C:\Program Files\LLVM\bin\clang.exe" if os.name == "nt" else "clang",
)

# Outside the tree, and different on every machine. The tree is checked out
# inside the development root on both -- `F:\RISCOSDEV\ROSASM` on one,
# `/Volumes/S/RISCOSDEV/ROSASM` on the other -- so the root is one directory
# up, and asking the filesystem beats writing either path down. A drive
# letter as the default was not merely wrong on a Mac, it was unreachable:
# every path built from it began `F:\RISCOSDEV/`, which no amount of
# extracting sources into the right place could satisfy.
DEVROOT = _env("RISCOSDEV", os.path.dirname(ROOT))
ROMS = _env("RISCOS_ROMS", os.path.join(DEVROOT, "roms"))
RISCOS_SRC = _env("RISCOS_SRC", os.path.join(DEVROOT, "riscos-src"))
BUILDHOST = _env("BUILDHOST", os.path.join(RISCOS_SRC, "BuildHost"))
RPCEMU = _env("RPCEMU", os.path.join(DEVROOT, "rpcemu", "win32", "RPCEmu"))
EMU = _env("RPCEMU_BIN", os.path.join(RPCEMU, "rpcemu-headless" + EXE))
# Beside the emulator's build, not inside it.
FARM = _env("ROSASM_FARM", os.path.join(DEVROOT, "rpcemu", "farm"))

# farm.py hands each worker its own hostfs through this variable, so it has
# to be read here rather than assumed from RPCEMU -- otherwise every worker
# stages into the one directory and they overwrite each other's units.
HOSTFS = _env("ROSASM_HOSTFS", os.path.join(RPCEMU, "hostfs"))
