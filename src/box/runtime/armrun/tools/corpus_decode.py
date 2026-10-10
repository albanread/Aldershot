#!/usr/bin/env python3
"""corpus_decode.py -- the ARM container's corpus verdict (design 25; SPRINTS.md A2).

corpus_scan.py's word survey could not tell code from data: every
category showed in every binary.  This is the decoding survey that
replaces it as the verdict:

  1. every &FF8 AIF image under the given directories is unsqueezed by
     running its own decompressor under the engine (armscan unsqueeze);
  2. its code is found by following control flow -- from the AIF entry,
     the direct branch targets, and every APCS function prologue
     (MOV ip, sp; STMDB sp!, {...}) -- so literal pools are not code;
  3. each instruction reached is classified by its encoding (the ARM
     ARM's, ARMv8 AArch32), and calls into the SharedCLibrary stub
     vectors (runs of MOV pc, #0) are named from roscc's library
     contract (clib-contract.json);
  4. every distinct instruction word reached is given to the engine
     alone (armscan probe), which says whether it translates it.

Output: corpus.json (everything), corpus.md (the table), under --out.

Usage:
    corpus_decode.py DIR [DIR ...] [--out DIR] [--armscan PATH] [--contract PATH] [--swis PATH]
"""

import argparse
import json
import re
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

LOAD = 0x8000

# ---- the encodings --------------------------------------------------------

def cond(w):
    return w >> 28


def is_branch(w):                 # B, BL (not BLX imm: cond 0xF)
    return (w & 0x0E000000) == 0x0A000000 and cond(w) != 0xF


def branch_target(at, w):
    off = w & 0x00FFFFFF
    if off & 0x800000:
        off -= 0x1000000
    return (at + 8 + off * 4) & 0xFFFFFFFF


def classify(at, w):
    """The categories an instruction falls in (a set of short names)."""
    c = set()
    cnd = cond(w)
    if cnd == 0xF:
        if (w & 0x0E000000) == 0x0A000000:
            c.add("blx-imm (to Thumb)")
        elif (w & 0xFE000000) == 0xF2000000 or (w & 0xFF100000) == 0xF4000000:
            c.add("neon")
        elif (w & 0xFD70F000) == 0xF550F000:
            c.add("pld")
        elif (w & 0x0E000000) in (0x0C000000, 0x0E000000) and ((w >> 8) & 0xF) in (1, 2):
            c.add("fpa (NV)")
        else:
            c.add("nv-condition (pre-ARMv5 'never')")
        return c
    if (w & 0x0F000000) == 0x0F000000:
        c.add("swi")
        return c
    if (w & 0x0E000000) == 0x0C000000 or (w & 0x0F000000) == 0x0E000000:
        cp = (w >> 8) & 0xF
        c.add("fpa" if cp in (1, 2) else "vfp" if cp in (10, 11) else f"coprocessor {cp}")
        return c
    if (w & 0x0E000000) == 0x0A000000:
        return c                                    # B, BL
    if (w & 0x0E000000) == 0x08000000:              # LDM/STM
        load, s, wb = (w >> 20) & 1, (w >> 22) & 1, (w >> 21) & 1
        regs, rn = w & 0xFFFF, (w >> 16) & 0xF
        if s and load and regs & 0x8000:
            c.add("ldm ^ with pc (26-bit return: restores the PSR)")
        elif s:
            c.add("ldm/stm ^ (user-bank registers)")
        if wb and regs & (1 << rn):
            c.add("ldm/stm writeback, base in list")
        if not load and regs & 0x8000 and not (w & 0x0FFFD800) == 0x092DD800:
            c.add("stm storing pc (not an APCS frame)")
        return c
    if (w & 0x0C000000) == 0x04000000:              # LDR/STR
        if (w & 0x02000010) == 0x02000010:
            c.add("armv6 media")
            return c
        p, wbit = (w >> 24) & 1, (w >> 21) & 1
        if not p and wbit:
            c.add("ldrt/strt")
        return c
    # 0x00000000-0x03FFFFFF: data processing and its neighbours
    if (w & 0x0FFFFFD0) == 0x012FFF10:
        c.add("bx/blx register")
        return c
    if (w & 0x0FB00FF0) == 0x01000090:
        c.add("swp")
        return c
    if (w & 0x0F8000F0) == 0x01800090 and ((w >> 8) & 0xF) == 0xF:
        c.add("ldrex/strex")
        return c
    if (w & 0x0F0000F0) == 0x00000090:
        if (w >> 20) & 1:
            c.add("muls (C flag: ARMv4 unpredictable)")
        return c
    if (w & 0x0E000090) == 0x00000090:
        c.add("ldrh/ldrsb/ldrd (ARMv4+)")
        return c
    if (w & 0x0FBF0FFF) == 0x010F0000:
        c.add("mrs")
        return c
    if (w & 0x0FB0F000) == 0x0320F000 or (w & 0x0FB0FFF0) == 0x0120F000:
        c.add("msr spsr" if (w >> 22) & 1 else
              "msr cpsr control (mode change)" if (w >> 16) & 1 else "msr cpsr flags")
        return c
    if (w & 0x0FFF0FF0) == 0x016F0F10:
        c.add("clz (ARMv5)")
        return c
    if (w & 0x0F9000F0) == 0x01000050:
        c.add("qadd family (ARMv5TE)")
        return c
    if (w & 0x0F900090) == 0x01000080:
        c.add("smulxy family (ARMv5TE)")
        return c
    if (w & 0x0C000000) == 0x00000000:
        op, s, rd = (w >> 21) & 0xF, (w >> 20) & 1, (w >> 12) & 0xF
        if 8 <= op <= 11 and rd == 15 and s:
            c.add("teqp/cmpp family (26-bit PSR write)")
        elif rd == 15 and s and not 8 <= op <= 11:
            c.add("movs pc (26-bit return: restores the PSR)")
    return c


def ends_flow(at, w):
    """Whether control cannot fall through to the next word."""
    if cond(w) != 0xE:
        return False
    if is_branch(w) and not (w >> 24) & 1:
        return True                                 # B
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        return True                                 # BX
    if (w & 0x0C500000) == 0x04100000 and ((w >> 12) & 0xF) == 15:
        return True                                 # LDR pc
    if (w & 0x0E108000) == 0x08108000:
        return True                                 # LDM {.., pc}
    if (w & 0x0C000000) == 0 and ((w >> 12) & 0xF) == 15 and not 8 <= (w >> 21) & 0xF <= 11 \
            and (w & 0x0E000090) != 0x00000090:
        return True                                 # data processing into pc
    if (w & 0x0F000000) == 0x0F000000 and ((w & 0xFDFFFF) == 0x11 or (w & 0xFFFFFF) in (0x2B, 0x41506)):
        return True                                 # OS_Exit; non-X OS_GenerateError, MessageTrans_ErrorLookup
    return False


def literal(at, w):
    """The address an LDR Rd, [pc, #+/-imm] reads, else None."""
    if (w & 0x0F7F0000) == 0x051F0000:              # LDR(B) immediate, P=1 W=0, Rn=pc
        imm = w & 0xFFF
        return at + 8 + (imm if (w >> 23) & 1 else -imm)
    return None

def ascii_word(w):
    """All four bytes printable or NUL: text, never an AL instruction (top byte &Ex)"""
    return all(b == 0 or 0x20 <= b < 0x7F for b in w.to_bytes(4, "little"))


def implausible(words_at, a):
    """Where a walk has run off code into data: a name or string, a function-name
    marker (&FF0000nn, which precedes a function: the name is before it), an
    NV-condition word, or a coprocessor RISC OS code never uses."""
    w = words_at(a)
    if (w & 0xFFFFFF00) == 0xFF000000:
        return True
    if ascii_word(w) and ascii_word(words_at(a + 4)):
        return True
    if cond(w) == 0xF and not (w & 0x0E000000) == 0x0A000000 and not (w & 0xFD70F000) == 0xF550F000 \
            and not (w & 0xFE000000) == 0xF2000000 and not (w & 0xFF100000) == 0xF4000000:
        return True
    if ((w & 0x0E000000) == 0x0C000000 or (w & 0x0F000000) == 0x0E000000) and cond(w) != 0xF \
            and ((w >> 8) & 0xF) not in (1, 2, 10, 11, 15):
        return True
    return False

# ---- one image ------------------------------------------------------------

def stub_runs(words):
    """Runs of MOV pc, #0 -- the stub entry vectors, unfilled: (address, length)"""
    runs, i = [], 0
    while i < len(words):
        if words[i] == 0xE3A0F000:
            j = i
            while j < len(words) and words[j] == 0xE3A0F000:
                j += 1
            if j - i >= 4:
                runs.append((LOAD + 4 * i, j - i))
            i = j
        else:
            i += 1
    return runs


def name_stubs(runs, chunks):
    """Each run to the contract's chunk it fits, in order: {address: name}"""
    names, ci = {}, 0
    order = [c for c in chunks]
    for addr, n in runs:
        while ci < len(order) and len(order[ci]["entries"]) < n:
            ci += 1
        if ci == len(order):
            break
        for k in range(n):
            names[addr + 4 * k] = order[ci]["entries"][k]
        ci += 1
    return names


def survey(img, info, chunks):
    data = Path(img).read_bytes()
    n = len(data) // 4
    words = struct.unpack_from(f"<{n}I", data)
    ro_end = LOAD + min(info.get("ro") or len(data), len(data))
    at_word = lambda a: words[(a - LOAD) // 4] if LOAD <= a < LOAD + 4 * n else 0
    in_code = lambda a: LOAD <= a < ro_end and not a & 3
    stubs = name_stubs(stub_runs(words), chunks)
    seeds = []
    for hdr in (0x8, 0xC):                          # zero-init, entry
        w = words[hdr // 4]
        if is_branch(w):
            seeds.append(branch_target(LOAD + hdr, w))
    for i in range(min(n, (ro_end - LOAD) // 4) - 1):
        if words[i] == 0xE1A0C00D and (words[i + 1] & 0xFFFF0000) == 0xE92D0000:
            seeds.append(LOAD + 4 * i)
    reached, data_words, calls, stopped = set(), set(), Counter(), 0
    todo = list(seeds)
    while todo:
        a = todo.pop()
        while in_code(a) and a not in reached and a not in data_words and a not in stubs:
            if implausible(at_word, a):
                stopped += 1
                break
            w = at_word(a)
            reached.add(a)
            lit = literal(a, w)
            if lit is not None:
                data_words.add(lit & ~3)
            if is_branch(w):
                t = branch_target(a, w)
                if t in stubs:
                    calls[stubs[t]] += 1
                elif in_code(t):
                    todo.append(t)
            if ends_flow(a, w):
                break
            a += 4
    reached -= data_words
    cats, swis = Counter(), Counter()
    for a in reached:
        w = at_word(a)
        for k in classify(a, w):
            cats[k] += 1
        if "swi" in classify(a, w):
            swis[w & 0xFDFFFF] += 1
    return {
        "reached": len(reached),
        "walks_stopped_at_data": stopped,
        "ro_words": (ro_end - LOAD) // 4,
        "categories": dict(cats),
        "swis": {str(k): v for k, v in swis.items()},
        "library_calls": dict(calls),
        "stub_runs": stub_runs(words),
        "words": sorted({at_word(a) for a in reached}),
        "fpa_words": sorted({at_word(a) for a in reached if "fpa" in classify(a, at_word(a))}),
    }

# ---- the engine's view ----------------------------------------------------

def probe_all(armscan, words):
    """Each word run alone by the engine: {word: result}; restarted after an abort"""
    results, todo = {}, list(words)
    while todo:
        p = subprocess.Popen([armscan, "probe"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        out, _ = p.communicate("".join(f"{w:08X}\n" for w in todo))
        last = None
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2:
                results[int(parts[0], 16)] = parts[1]
            elif len(parts) == 1:
                last = int(parts[0], 16)            # named, then the engine died on it
        if last is not None:
            results[last] = "abort"
        todo = [w for w in todo if w not in results]
        if p.returncode == 0 and todo:
            for w in todo:
                results[w] = "unanswered"
            break
    return results

# ---- the corpus -----------------------------------------------------------

LIB_FILE = {"fopen", "freopen", "fread", "fwrite", "fgetc", "fputc", "fgets", "fputs", "_filbuf", "_flsbuf",
            "fprintf", "fscanf", "fclose", "remove", "rename", "tmpfile"}
LIB_FP = {"sin", "cos", "tan", "atan", "atan2", "exp", "log", "log10", "pow", "sqrt", "floor", "ceil",
          "fabs", "fmod", "frexp", "ldexp", "modf", "atof", "strtod", "_ldfp", "_stfp", "asin", "acos"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="+", type=Path)
    here = Path(__file__).resolve().parents[3]       # rosgd
    ap.add_argument("--out", type=Path, default=here / "build/armrun/corpus")
    ap.add_argument("--armscan", default=str(here / "build/armrun/armscan"))
    ap.add_argument("--contract", type=Path, default=here.parent / "roscc/tools/clibspec/clib-contract.json")
    ap.add_argument("--swis", type=Path, default=None, help="a swis.h to name SWIs from")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    chunks = json.loads(args.contract.read_text())["chunks"]
    swinames = {}
    if args.swis and args.swis.exists():
        for m in re.finditer(r"#define\s+(\w+)\s+0x([0-9A-Fa-f]+)", args.swis.read_text()):
            swinames.setdefault(int(m.group(2), 16), m.group(1))

    files = sorted(f for d in args.dirs for f in d.rglob("*,ff8"))
    corpus = []
    for f in files:
        rel = str(f)
        img = args.out / (re.sub(r"[^A-Za-z0-9]+", "_", rel)[-80:] + ".img")
        r = subprocess.run([args.armscan, "unsqueeze", str(f), str(img)], capture_output=True, text=True)
        try:
            info = json.loads(r.stdout.strip().splitlines()[-1])
        except (IndexError, json.JSONDecodeError):
            info = {"file": rel, "ok": False, "why": (r.stderr or r.stdout)[-200:]}
        row = {"file": rel, "unsqueeze": info}
        if info.get("ok") and info.get("aif"):
            row.update(survey(img, info, chunks))
        corpus.append(row)
        print(f"{'ok ' if 'reached' in row else '-- '} {rel}", file=sys.stderr)

    words = sorted({w for row in corpus for w in row.get("words", [])})
    print(f"probing {len(words)} distinct instruction words", file=sys.stderr)
    probed = probe_all(args.armscan, words)
    for row in corpus:
        ws = row.pop("words", [])
        bad = Counter(probed.get(w, "unprobed") for w in ws)
        bad.pop("ok", None)
        fpa = set(row.get("fpa_words", []))
        row["engine"] = dict(bad)
        row["engine_non_fpa"] = sorted(f"{w:08X}:{probed.get(w)}" for w in ws
                                       if probed.get(w) != "ok" and w not in fpa)[:40]
        row.pop("fpa_words", None)
        swi = {int(k) for k in row.get("swis", {})}
        calls = row.get("library_calls", {})
        row["runtime"] = ("SharedCLibrary APCS-32" if swi & {0x80683} else
                          "SharedCLibrary APCS-R (26-bit)" if swi & {0x80681} else
                          "UnixLib" if any(0x55C80 <= s < 0x55CC0 for s in swi) else "none (no library seen)")
        row["wimp"] = 0x400C0 in swi
        row["file_io"] = sorted(LIB_FILE & set(calls))
        row["fp_library"] = sorted(LIB_FP & set(calls))
        row["swi_names"] = sorted(swinames.get(s, f"&{s:X}") for s in swi)
    (args.out / "corpus.json").write_text(json.dumps({"probed": {f"{w:08X}": v for w, v in probed.items()},
                                                      "binaries": corpus}, indent=1))

    # the table
    lines = ["| Binary | Squeezed | Runtime | Wimp | Reached / RO words | FPA | VFP | NEON | Old idioms | Engine refuses (non-FPA) | File I/O | FP library |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    old_keys = ("movs pc", "ldm ^ with pc", "teqp", "ldm/stm ^ (user", "swp", "nv-condition", "muls", "ldrt")
    for row in corpus:
        name = row["file"].split("v3-test-disc/")[-1]
        if "reached" not in row:
            why = row["unsqueeze"].get("why") or "not AIF"
            lines.append(f"| {name} | | {why} | | | | | | | | | |")
            continue
        cats = row["categories"]
        old = sum(v for k, v in cats.items() if k.startswith(old_keys))
        lines.append(
            f"| {name} | {'yes' if row['unsqueeze']['squeezed'] else ''} | {row['runtime']} | {'yes' if row['wimp'] else ''} "
            f"| {row['reached']} / {row['ro_words']} | {cats.get('fpa', 0)} | {cats.get('vfp', 0)} | {cats.get('neon', 0)} "
            f"| {old or ''} | {len(row['engine_non_fpa']) or ''} | {', '.join(row['file_io'])} | {', '.join(row['fp_library'])} |")
    totals = Counter()
    for row in corpus:
        for k, v in row.get("categories", {}).items():
            totals[k] += v
    lines += ["", "Categories over the corpus (instructions reached):", ""]
    lines += [f"- {k}: {v}" for k, v in totals.most_common()]
    refusals = Counter(v for v in probed.values() if v != "ok")
    lines += ["", f"Engine, distinct words probed: {len(probed)}; not ok: {dict(refusals)}"]
    (args.out / "corpus.md").write_text("\n".join(lines) + "\n")
    print(args.out / "corpus.md")


if __name__ == "__main__":
    main()
