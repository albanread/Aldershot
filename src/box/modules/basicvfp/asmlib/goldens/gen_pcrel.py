#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read
"""gen_pcrel.py: goldens for the PC-relative forms. In BASIC the operand of
these forms is the address of the target. Each instruction is assembled by
LLVM MC (the integrated assembler in Xcode's clang). Its target is a label
at a known offset. The golden is written out with that label's address.
The page arithmetic of adrp is resolved only by a link. Its cases are
therefore linked at a page-aligned base (&100000000) and written relative
to it.

    gen_pcrel.py DIR      writes DIR/pcrel_a64.tsv and DIR/pcrel_x86_64.tsv

The files are in the format asmtest reads (the Makefile's testpcrel). Each
line has the text, the bytes, and then @address, which is where the
instruction is assembled. For x86-64, [rip + T] is written with the target
as a name, L_<hex>. That is a label, as a BASIC variable is. A plain number
there is a displacement, as it is for MC."""
import os
import subprocess
import sys
import tempfile

BASE = 0x10000          # where offset 0 of the object stands
A64 = [                 # (instruction with {T} for the target, address, target), offsets from BASE
    ("b {T}", 0x100, 0x200), ("b {T}", 0x100, 0x40), ("b {T}", 0x100, 0x100),
    ("bl {T}", 0x100, 0x3000), ("bl {T}", 0x2000, 0x100),
    ("b.eq {T}", 0x100, 0x200), ("b.ne {T}", 0x200, 0x100), ("b.lt {T}", 0x100, 0x104),
    ("cbz x3, {T}", 0x100, 0x180), ("cbz w3, {T}", 0x180, 0x100),
    ("cbnz x9, {T}", 0x100, 0x1100), ("tbz w5, #3, {T}", 0x100, 0x120),
    ("tbnz x7, #40, {T}", 0x200, 0x100), ("tbnz w0, #31, {T}", 0x100, 0x2100),
    ("adr x1, {T}", 0x100, 0x131), ("adr x2, {T}", 0x100, 0x55), ("adr x30, {T}", 0x100, 0x100),
    ("ldr x2, {T}", 0x100, 0x200), ("ldr w2, {T}", 0x200, 0x100), ("ldrsw x4, {T}", 0x100, 0x1000),
    ("ldr s1, {T}", 0x100, 0x140), ("ldr d2, {T}", 0x100, 0x148), ("ldr q7, {T}", 0x100, 0x150),
    ("prfm pldl1keep, {T}", 0x100, 0x200),
]
A64_ADRP = [(0x2000, 0x4000), (0x2004, 0x7008), (0x2008, 0x2FFF), (0x3000, 0x2010), (0x2010, 0x2000)]
X86 = [
    ("jmp {T}", 0x100, 0x110), ("jmp {T}", 0x100, 0x400), ("jmp {T}", 0x200, 0x1F0),
    ("jmp {T}", 0x400, 0x100), ("call {T}", 0x100, 0x400), ("call {T}", 0x400, 0x100),
    ("je {T}", 0x100, 0x120), ("jne {T}", 0x100, 0x800), ("jl {T}", 0x400, 0x100),
    ("lea rax, [rip + {T}]", 0x100, 0x400), ("mov eax, dword ptr [rip + {T}]", 0x100, 0x80),
    ("mov dword ptr [rip + {T}], 5", 0x100, 0x200), ("movsd xmm0, qword ptr [rip + {T}]", 0x100, 0x300),
]


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{' '.join(args)}: {r.stderr}")
    return r.stdout


def text_of(path, arm):
    """__text's bytes, by address"""
    data = {}
    for line in run(["otool", "-X", "-t", path]).splitlines():
        f = line.split()
        if not f:
            continue
        a = int(f[0], 16)
        for w in f[1:]:
            if arm:
                for i, b in enumerate(int(w, 16).to_bytes(4, "little")):
                    data[a + i] = b
                a += 4
            else:
                data[a] = int(w, 16)
                a += 1
    return data


def mc(triple, lines, link=False):
    """__text's bytes by address, and the symbols' addresses"""
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, "t.s"), os.path.join(d, "t")
        if triple.startswith("x86"):
            lines = [".intel_syntax noprefix"] + lines
        open(src, "w").write("\n".join(lines) + "\n")
        args = ["clang", "-target", triple, "-o", out, src]
        args += ["-nostdlib", "-static", "-Wl,-e,_start"] if link else ["-c"]
        run(args)
        syms = {}
        for line in run(["nm", "-a", out]).splitlines():
            f = line.split()
            if len(f) == 3:
                syms[f[2]] = int(f[0], 16)
        return text_of(out, triple.startswith("arm64")), syms


def placed(ins, at, to):
    """the instruction at offset at, its label L_t at offset to"""
    lines = ["L_0:"]
    if to < at:
        lines += [f".org {to:#x}", "L_t:", f".org {at:#x}", "  " + ins.replace("{T}", "L_t")]
    elif to == at:
        lines += [f".org {at:#x}", "L_t:", "  " + ins.replace("{T}", "L_t")]
    else:
        lines += [f".org {at:#x}", "  " + ins.replace("{T}", "L_t"), "ins_end:", f".org {to:#x}", "L_t:"]
    if to <= at:
        lines.append("ins_end:")
    return lines + ["  .space 16"]


def main(d):
    with open(os.path.join(d, "pcrel_a64.tsv"), "w") as f:
        f.write("# AArch64's PC-relative forms, the operand the target's address as BBC BASIC\n"
                "# gives it: LLVM MC's bytes (gen_pcrel.py, Xcode's clang); @ where assembled\n")
        for ins, at, to in A64:
            b, _ = mc("arm64-apple-macos11", placed(ins, at, to))
            word = bytes(b[at + i] for i in range(4))
            f.write(f"{ins.replace('{T}', hex(BASE + to))}\t{word.hex()}\t@{BASE + at:x}\n")
        for at, to in A64_ADRP:
            lines = [".globl _start", ".p2align 12", "_start:"]
            off = 0x2000
            for pos, what in sorted([(at, "i"), (to, "t")]):
                lines.append(f"  .space {pos - off:#x}")
                off = pos
                if what == "i":
                    lines.append("  adrp x0, L_t@PAGE")
                    off += 4
                else:
                    lines.append("L_t:")
            lines.append("  ret")
            b, syms = mc("arm64-apple-macos11", lines, link=True)
            start = syms["_start"]                  # page-aligned, and it stands for &2000
            word = bytes(b[start + at - 0x2000 + i] for i in range(4))
            f.write(f"adrp x0, {to:#x}\t{word.hex()}\t@{at:x}\n")
    with open(os.path.join(d, "pcrel_x86_64.tsv"), "w") as f:
        f.write("# x86-64's PC-relative forms, the operand the target's address as BBC BASIC\n"
                "# gives it: LLVM MC's bytes (gen_pcrel.py, Xcode's clang); @ where assembled\n")
        for ins, at, to in X86:
            b, syms = mc("x86_64-apple-macos11", placed(ins, at, to))
            size = syms["ins_end"] - at if to > at else None
            if size is None:
                # The end label stands after the target's. The length is
                # measured from the instruction's own first byte, which MC
                # relaxed.
                size = syms["ins_end"] - at
            t = f"L_{BASE + to:x}" if "[rip" in ins else hex(BASE + to)
            f.write(f"{ins.replace('{T}', t)}\t{bytes(b[at + i] for i in range(size)).hex()}"
                    f"\t@{BASE + at:x}\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
