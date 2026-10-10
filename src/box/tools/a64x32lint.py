#!/usr/bin/env python3
"""a64x32lint.py -- A64X32's addressing lint.

    tools/a64x32lint.py [--objdump PATH] [--show N] [--json FILE] OBJ...

A64X32 code keeps every data address below 4 GB.  -fwrapv-pointer makes
clang form a pointer sum in a W register, which wraps at 4 GB as x32's
addr32 does; what the back end still folds into a 64-bit addressing mode is
a value below 4 GB plus an offset the instruction bounds, and the runtime
reserves [4 GB, 68 GB) PROT_NONE, so such an access faults and cannot reach
the runtime above.  This checks that invariant in the objects: it
disassembles each (ELF32, EM_AARCH64; llvm-objdump) and bounds each load's
and store's effective address from the instructions that formed it.  An
address that can reach 68 GB or more is unbounded, and the lint fails.

How an address is bounded.  Walking back in the basic block from the
access, each register's definition gives its bound:

    a W register written (any instruction)       < 2^32, zero-extended
    sp, adrp/adr (a symbol: images are linked     < 2^32
      below 4 GB), mrs TPIDR_EL0 (roscc makes it
      x18, the static base, in the arena)
    add/sub x, x, #imm                           the register's bound + imm
    add/sub x, x, x{, lsl #s}                    the sum of the bounds
    add/sub x, x, w, uxtw|sxtw{ #s}              + 2^32 << s (sxtw: 2^31 << s, either sign)
    orr x, x, #imm or a register                 the sum of the bounds (a | b <= a + b)
    mov/csel/and/ubfx/ubfiz/bfi/lsl x ...        as the instruction bounds it
    a call (bl, blr)                             x0-x18 < 2^32: A64X32 functions return
                                                 pointers zero-extended
    none in the block (live in)                  < 2^32: between blocks a pointer is a
                                                 32-bit value (ILP32); a 64-bit loop
                                                 induction (add x8, x8, #4) walks in
                                                 steps an instruction bounds, so it meets
                                                 the reservation before it can pass it
    ldr x from [sp..] or [x29..]                 < 2^32: a spill, or an argument passed in
                                                 memory -- 8-byte slots, which A64X32 code
                                                 fills with pointers zero-extended (a
                                                 structure passed by reference is its
                                                 address in one; the callee loads 64 bits)
    anything else (a 64-bit load, mul, movk,     unbounded
      eor, fmov ...)

and the memory operand adds its offset: #imm, a register's bound shifted,
or a W register extended.  sxtw's negative reach wraps below 0 to the top
of the address space, which EL0 cannot touch: it faults too.

Also counted, as design 20 A.3's clampcheck.py counted them (the operand's
form only): stack-based ([sp..], [x29..]), plain [xN], register offset,
immediate offset and writeback; and the objects' text bytes.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys

TWO32 = 1 << 32
LIMIT = 68 << 30            # 4 GB + the 64 GB PROT_NONE reservation
INF = float("inf")

MEM = re.compile(r"\b(ldr|ldrb|ldrh|ldrsb|ldrsh|ldrsw|ldur\w*|ldtr\w*|str|strb|strh|stur\w*|sttr\w*|"
                 r"ldp|ldpsw|stp|ldnp|stnp|ld[1-4]r?|st[1-4]|ldar\w*|ldapr\w*|stlr\w*|stllr\w*|ldlar\w*|"
                 r"ldx\w*|stx\w*|ldax\w*|stlx\w*|prfm|prfum|ld(add|clr|eor|set|smax|smin|umax|umin)\w*|"
                 r"st(add|clr|eor|set|smax|smin|umax|umin)\w*|swp\w*|cas\w*)$")
NODEF = re.compile(r"^(str\w*|stur\w*|sttr\w*|stp|stnp|st[1-4]|stlr\w*|stllr\w*|prfm|prfum|"
                   r"st(add|clr|eor|set|smax|smin|umax|umin)\w*|cmp|cmn|tst|ccmp|ccmn|fcmp\w*|"
                   r"b|b\.\w+|bc\.\w+|br|blr|bl|ret|cbz|cbnz|tbz|tbnz|nop|brk|hint|dmb|dsb|isb|msr|"
                   r"hlt|udf|svc|hvc|pacia\w*|autia\w*|bti)$")
BRANCH = re.compile(r"^(b|b\.\w+|bc\.\w+|cbz|cbnz|tbz|tbnz)$")
ENDS = re.compile(r"^(b|b\.\w+|bc\.\w+|br|ret|cbz|cbnz|tbz|tbnz|brk|udf)$")
XREG = re.compile(r"^x(\d+)$")
WREG = re.compile(r"^w(\d+)$")


def elf32_aarch64(path):
    """(is ELF32 EM_AARCH64, text bytes)"""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF":
        return False, 0
    if d[4] != 1:
        return False, 0
    machine, = struct.unpack_from("<H", d, 0x12)
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    text = 0
    for i in range(shnum):
        _, typ, flags, _, _, size = struct.unpack_from("<IIIIII", d, shoff + i * shentsize)
        if typ == 1 and flags & 4:          # SHT_PROGBITS, SHF_EXECINSTR
            text += size
    return machine == 183, text


def split_ops(s):
    """operands, split at top-level commas: '[x0, #4]!' stays one"""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def imm(s):
    s = s.strip().lstrip("#")
    try:
        return int(s, 0)
    except ValueError:
        return None


def regname(op):
    m = re.match(r"^([xw])(\d+)$", op)
    if m:
        return m.group(1), int(m.group(2))
    if op in ("sp", "wsp"):
        return op[0] if op == "wsp" else "x", "sp"
    if op in ("xzr", "wzr"):
        return op[0], "zr"
    return None, None


class Insn:
    __slots__ = ("addr", "mn", "ops", "text", "reloc")

    def __init__(self, addr, mn, ops, text):
        self.addr, self.mn, self.ops, self.text, self.reloc = addr, mn, ops, text, False


def disassemble(objdump, path):
    """{function: [Insn]}"""
    out = subprocess.run([objdump, "-d", "-r", "--no-show-raw-insn", path],
                         capture_output=True, text=True, errors="replace")
    if out.returncode:
        raise SystemExit("a64x32lint: %s: %s" % (path, out.stderr.strip()[-300:]))
    funcs, cur, last = {}, None, None
    for line in out.stdout.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
        if m:
            cur = funcs.setdefault(m.group(1), [])
            continue
        if cur is None:
            continue
        if re.match(r"^\s+[0-9a-f]+:\s+R_AARCH64_", line):
            if last is not None:
                last.reloc = True
            continue
        # (an object's addresses are space-padded; a linked image's, at
        # &8000 or &72200000, fill the field and start the line)
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
        if not m:
            continue
        ops = m.group(3).split("//")[0].strip()
        last = Insn(int(m.group(1), 16), m.group(2), ops, line.strip())
        cur.append(last)
    return funcs


def leaders(insns):
    """the indexes that start a basic block"""
    at = {i.addr: k for k, i in enumerate(insns)}
    lead = {0}
    for k, i in enumerate(insns):
        if ENDS.match(i.mn):
            lead.add(k + 1)
        if BRANCH.match(i.mn) and not i.reloc:
            m = re.search(r"\b0x([0-9a-f]+) <", i.ops)
            if m and int(m.group(1), 16) in at:
                lead.add(at[int(m.group(1), 16)])
    return lead


class Bounder:
    """bounds a register's value at an instruction, from its definition in the block"""

    def __init__(self, insns, lead):
        self.insns, self.lead = insns, lead

    def defs(self, i, reg):
        """does insn i write register reg (a number, or 'sp')?  -> 'x', 'w' or None"""
        if NODEF.match(i.mn):
            # a writeback store updates its base
            return self.wb_base(i, reg)
        ops = split_ops(i.ops)
        if not ops:
            return None
        dests = [ops[0]]
        if i.mn in ("ldp", "ldpsw", "ldnp", "ldaxp", "ldxp"):
            dests.append(ops[1])
        for d in dests:
            w, n = regname(d)
            if n == reg:
                return w
        return self.wb_base(i, reg)

    @staticmethod
    def wb_base(i, reg):
        if not MEM.match(i.mn):
            return None
        m = re.search(r"\[(\w+)(.*?)\](!?)(.*)$", i.ops)
        if not m or not (m.group(3) or m.group(4).strip().startswith(",")):
            return None
        w, n = regname(m.group(1))
        return "wb" if n == reg else None

    def bound(self, k, reg, depth=0, why=None):
        """the bound of 64-bit register reg (number or 'sp') just before insn k"""
        if reg == "sp" or reg == "zr":
            return TWO32 if reg == "sp" else 1
        if depth > 12:
            return self.note(why, INF, "too deep")
        j = k - 1
        while j >= 0 and (j + 1) not in self.lead:
            i = self.insns[j]
            if i.mn in ("bl", "blr"):
                if isinstance(reg, int) and reg <= 18:
                    return self.note(why, TWO32, "call result")
            d = self.defs(i, reg)
            if d:
                return self.of_def(j, i, d, reg, depth, why)
            j -= 1
        return self.note(why, TWO32, "live in")

    @staticmethod
    def note(why, b, s):
        if why is not None:
            why.append(s)
        return b

    def operand(self, k, op, depth, why):
        """bound of a source operand: register, shifted or extended register, immediate"""
        parts = [p.strip() for p in op.split(",")]
        base = parts[0]
        v = imm(base)
        if v is not None:
            return abs(v)
        w, n = regname(base)
        if n is None:
            return self.note(why, INF, "operand " + op)
        b = TWO32 if w == "w" else self.bound(k, n, depth + 1, why)
        if len(parts) > 1:
            m = re.match(r"(lsl|uxtw|sxtw|uxtx|sxtx|uxtb|uxth|sxtb|sxth)\s*#?(\d*)", parts[1])
            if not m:
                return self.note(why, INF, "shift " + parts[1])
            s = int(m.group(2) or 0)
            kind = m.group(1)
            if kind == "uxtb":
                b = 256
            elif kind == "uxth":
                b = 65536
            elif kind == "uxtw":
                b = TWO32
            elif kind == "sxtw":
                b = 1 << 31
            elif kind in ("sxtb", "sxth"):
                b = 1 << (7 if kind == "sxtb" else 15)
            b = b * (1 << s)
        return b

    def of_def(self, j, i, d, reg, depth, why):
        mn, ops = i.mn, split_ops(i.ops)
        if d == "w":
            return self.note(why, TWO32, "W-formed (%s)" % i.text.split(":", 1)[1].strip())
        if d == "wb":
            # a writeback's new base: base + its immediate (a walk, as above)
            m = re.search(r"#(-?0x[0-9a-f]+|-?\d+)", ops[-1] if ops else "")
            step = abs(imm(m.group(1))) if m else INF
            return self.bound(j, reg, depth + 1, why) + step
        src = ops[1:]
        if mn == "mov" and src:
            if src[0] == "sp":
                return TWO32
            v = imm(src[0])
            if v is not None:
                return self.note(why, abs(v), "constant")
            return self.operand(j, src[0], depth, why)
        if mn in ("add", "adds", "sub", "subs") and len(src) >= 2:
            if src[0] == "sp":
                a = TWO32
            else:
                a = self.operand(j, src[0], depth, why)
            rest = ", ".join(src[1:])
            v = imm(src[1]) if len(src) == 2 or src[2].startswith("lsl") else None
            if v is not None:
                sh = re.search(r"lsl\s*#(\d+)", rest)
                return a + abs(v) * (1 << int(sh.group(1)) if sh else 1)
            return a + self.operand(j, rest, depth, why)
        if mn in ("ldr", "ldur", "ldp") and re.search(r"\[(sp|x29)\b", i.ops):
            # a register spilled, or an argument passed in memory: 8-byte
            # slots, which A64X32 code fills with pointers zero-extended
            # (a structure passed by reference is its address in one)
            return self.note(why, TWO32, "stack slot (%s)" % i.text.split(":", 1)[1].strip())
        if mn in ("adrp", "adr"):
            return self.note(why, TWO32, "symbol")
        if mn == "mrs" and len(src) == 1 and src[0].upper() == "TPIDR_EL0":
            return self.note(why, TWO32, "thread pointer (x18)")
        if mn in ("csel", "csinc", "csneg", "csinv") and len(src) >= 2:
            if mn in ("csneg", "csinv"):
                return self.note(why, INF, mn)
            return max(self.operand(j, src[0], depth, why), self.operand(j, src[1], depth, why) + 1)
        if mn in ("and", "ands") and len(src) == 2 and imm(src[1]) is not None:
            v = imm(src[1])
            if v >= 0:
                return v + 1
        if mn in ("ubfx",) and len(src) == 3:
            return 1 << imm(src[2])
        if mn in ("ubfiz",) and len(src) == 3:
            return 1 << (imm(src[1]) + imm(src[2]))
        if mn in ("bfi", "bfxil") and len(src) == 3:
            # bits inserted into the register's old value
            return self.bound(j, reg, depth + 1, why) + (1 << (imm(src[1]) + imm(src[2])))
        if mn in ("lsl",) and len(src) == 2 and imm(src[1]) is not None:
            return self.operand(j, src[0], depth, why) * (1 << imm(src[1]))
        if mn in ("lsr",) and len(src) == 2 and imm(src[1]) is not None:
            return self.operand(j, src[0], depth, why)
        if mn in ("sxtw",) and len(src) == 1:
            return self.note(why, 1 << 31, "sxtw")
        if mn in ("sbfiz",) and len(src) == 3:
            return 1 << (imm(src[1]) + imm(src[2]) - 1)
        if mn == "orr" and len(src) == 2 and src[0] in ("xzr",):
            return self.operand(j, src[1], depth, why)
        if mn == "orr" and len(src) >= 2:
            # a | b <= a + b (va_arg's `orr x10, x12, #0x8` over a W value)
            return self.operand(j, src[0], depth, why) + self.operand(j, ", ".join(src[1:]), depth, why)
        return self.note(why, INF, "64-bit %s (%s)" % (mn, i.text.split(":", 1)[1].strip()))


MEMOP = re.compile(r"\[(x\d+|sp)\b")


def memop(i):
    """the memory operand and what follows it (not ld1's lane index, {v0.s}[1])"""
    return i.ops[MEMOP.search(i.ops).start():]


def classify(i):
    """design 20 A.3's clampcheck.py form: (base, kind)"""
    mem = memop(i)
    base = re.match(r"\[(\w+)", mem).group(1)
    if base in ("sp", "x29"):
        return base, "stack"
    if re.search(r"\]!|\], #", mem):
        return base, "wb"
    if re.match(r"\[\w+, [xw]\d+", mem):
        return base, "regoff"
    if re.match(r"\[\w+, #", mem):
        return base, "immoff"
    return base, "plain"


def address_bound(b, k, i):
    """(bound, why) of insn k's effective address"""
    mem = memop(i)
    m = re.match(r"\[([^\]]*)\](!?)", mem)
    parts = [p.strip() for p in m.group(1).split(",")]
    why = []
    w, n = regname(parts[0])
    if n is None:
        return INF, ["base " + parts[0]]
    a = b.bound(k, n, 0, why)
    if len(parts) > 1:
        v = imm(parts[1])
        if v is not None:
            a += abs(v)
        else:
            a += b.operand(k, ", ".join(parts[1:]), 0, why)
    return a, why


def lint(objdump, objs):
    forms = dict(total=0, stack=0, plain=0, regoff=0, immoff=0, wb=0)
    reach = dict(stack=0, w_formed=0, bounded=0, unbounded=0)
    bad, notelf, text = [], [], 0
    for obj in objs:
        ok, t = elf32_aarch64(obj)
        if not ok:
            notelf.append(obj)
            continue
        text += t
        for fn, insns in disassemble(objdump, obj).items():
            if not insns:
                continue
            b = Bounder(insns, leaders(insns))
            for k, i in enumerate(insns):
                if not MEM.match(i.mn) or not MEMOP.search(i.ops):
                    continue
                forms["total"] += 1
                base, kind = classify(i)
                forms[kind] += 1
                if kind == "stack":
                    reach["stack"] += 1
                    continue
                a, why = address_bound(b, k, i)
                if a < LIMIT:
                    plainw = kind == "plain" and why and why[0].startswith(("W-formed", "live in", "call"))
                    reach["w_formed" if plainw and a <= TWO32 else "bounded"] += 1
                else:
                    reach["unbounded"] += 1
                    bad.append("%s: %s: %s  [%s]" % (os.path.basename(obj), fn, i.text, "; ".join(why)))
    return forms, reach, bad, notelf, text


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("objs", nargs="+")
    ap.add_argument("--objdump", default=None, help="llvm-objdump (default: .cache/llvm's, then PATH)")
    ap.add_argument("--show", type=int, default=20, help="unbounded accesses shown")
    ap.add_argument("--json", help="write the counts here")
    args = ap.parse_args()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    objdump = args.objdump or next((p for p in (os.path.join(here, ".cache/llvm/bin/llvm-objdump"),
                                                shutil.which("llvm-objdump")) if p and os.access(p, os.X_OK)), None)
    if not objdump:
        raise SystemExit("a64x32lint: no llvm-objdump (sh deps/build-llvm.sh)")
    forms, reach, bad, notelf, text = lint(objdump, args.objs)
    print("a64x32lint: %d objects, %d text bytes, %d loads and stores"
          % (len(args.objs) - len(notelf), text, forms["total"]))
    print("  forms (design 20 A.3): stack %(stack)d, plain %(plain)d, register offset %(regoff)d, "
          "immediate offset %(immoff)d, writeback %(wb)d" % forms)
    print("  reach: stack %(stack)d, W-formed %(w_formed)d, bounded 64-bit %(bounded)d, "
          "unbounded %(unbounded)d" % reach)
    for line in bad[:args.show]:
        print("  unbounded: " + line)
    for obj in notelf:
        print("  not ELF32 AArch64: " + obj)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(dict(objects=len(args.objs) - len(notelf), text=text, forms=forms, reach=reach,
                           unbounded=bad, not_elf32=notelf), f, indent=1)
    return 1 if bad or notelf else 0


if __name__ == "__main__":
    sys.exit(main())
