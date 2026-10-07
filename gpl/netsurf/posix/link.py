#!/usr/bin/env python3
"""link.py --musl DIR -o OUT OBJECT|ARCHIVE|-lNAME|-LDIR... -- a POSIX application (design 22).

Links x32 objects with musl (DIR: build_musl.py's output) into what the C
applications' loader runs (include/rosgd/capp.h): an ELF32 x86-64
executable at &8000 (posix.ld), its ROSGD note, and OS/ABI 255, which this
sets after zig's ld.lld has linked it.  Then checks what the loader will:
every loadable segment at &8000 or above and below 4 GB, the entry in
code, no interpreter, no dynamic section, no thread-local storage; and no
instruction anywhere using %fs or %gs.
$ZIG names zig (default: zig).

A64X32 (the Apple Silicon box, design 26), when DIR's file "abi" says
a64x32: ld.lld has no P32 relocations, so roscc links (link --entry
_start; $ROSCC, else ../roscc's release build): the same image at &8000,
EM_AARCH64, with the note and OS/ABI 255 roscc writes, every object
through its x18 and addressing checks.  Archives are taken as roscc takes
them, member by member as needed, wherever they stand.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ZIG = os.environ.get("ZIG", "zig")

PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_TLS = 1, 2, 3, 4, 7
PF_X = 1


def check(path, machine=62):
    with open(path, "r+b") as f:
        b = bytearray(f.read())
        if b[:4] != b"\x7fELF" or b[4] != 1 or b[5] != 1:
            raise SystemExit(f"link: {path} is not a little-endian ELF32 file")
        em, = struct.unpack_from("<H", b, 18)
        if em != machine:
            raise SystemExit(f"link: {path} is machine {em}, not {machine}")
        entry, phoff = struct.unpack_from("<II", b, 24)
        phentsize, phnum = struct.unpack_from("<HH", b, 42)
        code, note = [], False
        for i in range(phnum):
            ptype, off, vaddr, _, filesz, memsz, flags, _ = \
                struct.unpack_from("<IIIIIIII", b, phoff + i * phentsize)
            if ptype in (PT_INTERP, PT_DYNAMIC):
                raise SystemExit(f"link: {path} is dynamic; a POSIX application is static")
            if ptype == PT_TLS:
                raise SystemExit(f"link: {path} has thread-local storage (design 22, section 9)")
            if ptype == PT_NOTE:
                note = True
            if ptype == PT_LOAD and memsz:
                if vaddr < 0x8000 or vaddr + memsz + 0x2000 > 0xFFFFFFFF:
                    raise SystemExit(f"link: a segment of {path} is at &{vaddr:X}")
                if flags & PF_X:
                    code.append((vaddr, vaddr + memsz))
        if not note:
            raise SystemExit(f"link: {path} has no ROSGD note")
        if not any(lo <= entry < hi for lo, hi in code):
            raise SystemExit(f"link: {path}'s entry &{entry:X} is not in its code")
        b[7] = 255                                  # EI_OSABI: ROSGD
        f.seek(0)
        f.write(b)


ROSCC = os.environ.get("ROSCC", os.path.join(os.path.dirname(os.path.dirname(HERE)), "roscc",
                                              "target", "release", "roscc"))


def link_a64x32(a):
    m = a.musl
    cmd = [ROSCC, "link", "--entry", "_start", "-o", a.out, os.path.join(m, "crt1.o")]
    cmd += a.inputs + [os.path.join(m, "libc.a")]
    if a.map:
        cmd += ["--map", a.map]
    r = subprocess.run(cmd)
    if r.returncode:
        raise SystemExit(r.returncode)
    check(a.out, machine=183)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--musl", required=True)
    ap.add_argument("-o", dest="out", required=True)
    ap.add_argument("--map", help="write the linker's map here")
    ap.add_argument("--ld-opt", action="append", default=[], help="an option for ld.lld itself")
    ap.add_argument("inputs", nargs="+")
    a = ap.parse_args()
    m = a.musl
    try:
        with open(os.path.join(m, "abi")) as fh:
            if fh.read().strip() == "a64x32":
                return link_a64x32(a)
    except OSError:
        pass
    cmd = [ZIG, "ld.lld", "-m", "elf32_x86_64", "--no-pie", "-static", "-nostdlib",
           "--gc-sections", "-T", os.path.join(HERE, "posix.ld"), "-o", a.out,
           os.path.join(m, "crt1.o"), os.path.join(m, "note.o")]
    cmd += a.ld_opt
    cmd += ["--start-group"] + a.inputs + [os.path.join(m, "libc.a"), "--end-group"]
    if a.map:
        cmd += ["-Map", a.map]
    r = subprocess.run(cmd)
    if r.returncode:
        raise SystemExit(r.returncode)
    check(a.out)
    segments(a.out)


def segments(path):
    """No instruction may use %fs or %gs: %fs is the runtime's thread pointer,
    %gs design 20's static base (design 22, section 9).  roscc's checks do not
    run on an image lld links, so this looks at every instruction."""
    objdump = os.environ.get("OBJDUMP", "objdump")
    r = subprocess.run([objdump, "-d", "--no-show-raw-insn", path], capture_output=True, text=True)
    if r.returncode:
        print(f"link: {objdump} could not check {path}'s code for %fs and %gs", file=sys.stderr)
        return
    bad = [l.strip() for l in r.stdout.splitlines() if "%fs:" in l or "%gs:" in l]
    if bad:
        raise SystemExit(f"link: {path} uses %fs or %gs ({len(bad)} instructions), first: {bad[0]}")


if __name__ == "__main__":
    main()
