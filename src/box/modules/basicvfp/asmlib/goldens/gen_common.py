#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read
"""gen_common.py: goldens for instructions that the frozen corpora leave out.
The instructions are listed in common_a64.txt and common_x86_64.txt. They
include ret, mov between registers, 16-bit immediates, lock, loads of s, d
and q registers, and extended registers. Each one is assembled by LLVM MC,
which is the integrated assembler in Xcode's clang. The result is written
out in the form that asmtest reads as a corpus.

    gen_common.py a64 common_a64.txt > common_a64.tsv
    gen_common.py x64 common_x86_64.txt > common_x86_64.tsv
"""
import os
import subprocess
import sys
import tempfile


def main():
    arch, path = sys.argv[1], sys.argv[2]
    triple = "arm64-apple-macos11" if arch == "a64" else "x86_64-apple-macos11"
    print(f"# {os.path.basename(path)}, as LLVM MC encodes it (gen_common.py, Xcode's clang)")
    for ins in (line.rstrip("\n") for line in open(path)):
        if not ins.strip() or ins.startswith("#"):
            continue
        with tempfile.TemporaryDirectory() as d:
            src, obj = os.path.join(d, "t.s"), os.path.join(d, "t.o")
            open(src, "w").write((".intel_syntax noprefix\n" if arch == "x64" else "") + "  " + ins + "\n")
            r = subprocess.run(["clang", "-target", triple, "-c", src, "-o", obj],
                               capture_output=True, text=True)
            if r.returncode:
                sys.exit(f"gen_common.py: MC refuses {ins!r}: {r.stderr}")
            dump = subprocess.run(["otool", "-X", "-t", obj], capture_output=True, text=True,
                                  check=True).stdout
            b = bytearray()
            for line in dump.splitlines():
                for w in line.split()[1:]:
                    b += int(w, 16).to_bytes(4, "little") if arch == "a64" else bytes([int(w, 16)])
            print(f"{ins}\t{b.hex()}")


if __name__ == "__main__":
    main()
