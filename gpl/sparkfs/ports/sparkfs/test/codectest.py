#!/usr/bin/env python3
"""SparkFS's codecs in the box: every archive format opened, listed, copied out, compared.

    ports/sparkfs/test/codectest.py [--refs] [--out DIR] [--run-script S]
                                 the box (ROSGD_ARCH=aarch64: the Apple Silicon one)
    ports/sparkfs/test/codectest.py --refs-only
                                 only the archives against reference extractors
    ports/sparkfs/test/codectest.py --farm MACHINE [--record]
                                 the same on RISC OS 5.30 (a bigmacfarm machine;
                                 the dev system's !SparkFS); --record keeps its
                                 listings and file hashes (data/farm-530.json) and,
                                 once, the Spark archives its SparkFS wrote (data/530)
    --extra DIR                  also read the Spark archives a box wrote (its
                                 share's SFSTEST.W): box-*
    --only PREFIX                only the archives (and writes) named so

The archives, one or more per format SparkFS reads (archivers.py makes the
ones nothing on a Mac writes; --refs checks every one with an independent
extractor where one is installed: lhasa, unar, 7zz, cabextract):

    lzh-*        LHA: header levels 0, 1, 2; -lh0-, -lh4-, -lh5-      Lzh       &DDC
    zoo-*        Zoo 2.10: stored, LZW, lh5                            Zoo       &DDC
    arj-*        ARJ: stored, method 1, method 4                       ARJ       &DDC
    sit-*        StuffIt 1.5: stored, RLE90, LZW; a folder, a resource fork;
                 sit-both in McStuffit's both-forks mode; sit-nested a
                 folder in a folder                                    McStuffit &DDC
    pit-*        PackIt: PMag, PMa4 (Huffman); pit-both                McStuffit &DDC
    cpt, cpt-nested  Compact Pro (RLE)                                 McStuffit &DDC
    arcfs-*      ArcFS: stored, packed, compressed; RISC OS types      Spark     &DDC
    packdir      PackDir (LZW, 13 bits); RISC OS types                 PackdDir  &68E
    cpio-*       newc, odc, bin, made by bsdtar                        CPIO      &ABA
    cab-*        stored and MSZIP, made by gcab (data/)                Cab       &ABF
    nested       third_party's SparkFSApp/test/NestedSpark.arc         Spark     &DDC
    530-*        Spark and PK arc archives RISC OS 5.30's SparkFS wrote (data/530)

In the box (tests/lib/boxrun.py, eight archives a boot: with every archive
open at once SparkFS's memory runs out, as on 5.30): the ROM !SparkFS's Load,
then for each archive *FileInfo and *Ex of it and its directories, and
*Copy of all of it (~CFR) out to the share -- McStuffit's archives with
*McStuffitMode 2 (the data fork), or 3 (both forks, MacBinary) for the
*-both ones, set before the archive is first opened.  Then the write
round: *SparkFSCreate 1 (Spark) and 3 (PK arc: no directories) with each
compression method SparkFS writes (*SparkFSMethod), the source tree
copied in with *Copy, and back out.  A run that stops in an archive (an
error ends the Obey file) goes on from the next.  Here:

    <archive>  a directory (*FileInfo); every file copied out the
               archive's, byte for byte, by name (HostFS's ,xxx parted
               off; none is Text); RISC OS types where the archive has
               them; MacBinary's forks the archive's; and -- unless 5.30
               cannot read it deterministically (cpio-newc) -- the *Ex
               lines, any error and every copied file's SHA-1 the same as
               RISC OS 5.30's (data/farm-530.json).  Where SparkFS itself
               gets an archive wrong (5.30 too: the README's list) only
               5.30's results are compared.
    write-*    each archive the box's SparkFS wrote, read back in the box,
               and unar on the Mac extracts the source tree from it
    faults     no fault report on the serial console

Exit status 0 if everything was as expected.
"""

import argparse
import datetime
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SPARKFS = os.path.dirname(HERE)
ROSGD = os.path.dirname(os.path.dirname(SPARKFS))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROSGD, "tests", "lib"))
import archivers as A  # noqa: E402

DATA = os.path.join(HERE, "data")
GOLDEN = os.path.join(DATA, "farm-530.json")
FARM_DIR = os.environ.get("BIGMACFARM_DIR", os.path.join(ROSGD, "..", "..", "bigmacfarm"))
DEV_SPARKFS = os.path.join(ROSGD, "..", "..", "dev system", "share", "Utilities", "!SparkFS")
SUB = "SFSTEST"                       # everything under this on the share
WRITING = True                        # the write round (--only without "write" drops it)

# Spark (type 1) and PK arc (type 3) methods SparkFS writes: s/info's
# compressiontypes without ReadOnly
WRITES = [(1, 0), (1, 1), (1, 6), (1, 7), (1, 11), (1, 12), (3, 0), (3, 12), (3, 1), (3, 6)]


# ---------------------------------------------------------------- the data

def source():
    """The tree archived: (path, bytes, RISC OS type); directories end /"""
    rnd = random.Random(107)
    text = "".join(f"Line {i} of the text, the {('quick', 'lazy', 'brown')[i % 3]} fox {i * 7 % 13}.\n"
                   for i in range(1200)).encode()
    return [
        ("ReadMe", b"An archive made for SparkFS's codec tests.\nSecond line.\n", 0xFFF),
        ("Docs/", b"", None),
        ("Docs/Text", text, 0xFFF),
        ("Docs/Random", bytes(rnd.randrange(256) for _ in range(12000)), 0xFFD),
        ("Docs/Sub/", b"", None),
        ("Docs/Sub/Mixed", bytes(rnd.choice(b"abcdef\n") for _ in range(16000)), 0xFFD),
        ("Runs", b"a" * 900 + b"\x90" * 7 + b"bcd" * 40 + b"e" * 300, 0xFFD),
        ("Empty", b"", 0xFFD),
    ]


def files_of(entries, drop=()):
    return [(p, d) for p, d, *_ in entries if not p.endswith("/") and p.split("/")[-1] not in drop]


def archives(out, log, extra=None):
    """Every archive: a list of dicts -- name, leaf (the RISC OS name),
    type, path (on the host), expect ({path: bytes} the files SparkFS
    gives), types ({path: RISC OS type} where the archive says), mac (a
    McStuffit archive), faithful (False: compare with 5.30 only)"""
    src = source()
    files = files_of(src)
    nodirs = [(p, d) for p, d in files]
    typed = [(p, d, t) if t is not None else (p, d) for p, d, t in src]
    res = []

    def add(name, ftype, data, expect, **kw):
        path = os.path.join(out, f"{name},{ftype:03x}")
        if data is not None:
            with open(path, "wb") as fh:
                fh.write(data)
        res.append(dict(name=name, leaf=name.replace("-", "_"), type=ftype, path=path,
                        expect=dict(expect), types=kw.pop("types", None), mac=kw.pop("mac", False),
                        faithful=kw.pop("faithful", True), note=kw.pop("note", ""), refs=kw.pop("refs", []),
                        exdirs=kw.pop("exdirs", None), mode=kw.pop("mode", 2), vs530=kw.pop("vs530", True),
                        ignore=kw.pop("ignore", []), forks=kw.pop("forks", None)))

    dirs = [(p, d) for p, d, *_ in src]
    # LHA: level 0 has no directory entries; the rest have them
    for lvl, m in ((0, "-lh0-"), (0, "-lh5-"), (1, "-lh4-"), (1, "-lh5-"), (2, "-lh5-"), (2, "-lh0-")):
        ents = nodirs if lvl == 0 else dirs
        add(f"lzh-l{lvl}{m[1:4]}", 0xDDC, A.lzh(ents, lvl, m), files, refs=["lha", "7zz"])
    for m in (0, 1, 2):
        add(f"zoo-{m}", 0xDDC, A.zoo(dirs, m), files, refs=["unar"])
    for m in (0, 1, 4):
        add(f"arj-{m}", 0xDDC, A.arj(dirs, m), files, refs=["unar", "7zz"])
    rsrc = b"The resource fork of ReadMe. " * 8
    mac = [(p, d, rsrc) if p == "ReadMe" else (p, d) for p, d, *_ in src]
    # SparkFS's sitlist counts the files in a folder but not the folders
    # (5.30's too): a folder in a folder is listed at the top, empty.  The
    # StuffIt archives keep to one level of folders; sit-nested has two,
    # and is compared with 5.30 only.
    flat = [(p.replace("Docs/Sub/", "Docs/"), *rest) for p, *rest in mac if p != "Docs/Sub/"]
    for m in (0, 1, 2):
        add(f"sit-{m}", 0xDDC, A.stuffit(flat, m), files_of(flat), mac=True, refs=["unar"])
    # McStuffit's default, both forks as MacBinary: compared with 5.30
    add("sit-both", 0xDDC, A.stuffit(flat, 2), {}, mac=True, mode=3, faithful=False, exdirs=["Docs"],
        forks={p: (d, r[0] if r else b"") for p, d, *r in flat if not p.endswith("/")},
        note="McStuffitMode 3: both forks, MacBinary")
    add("sit-nested", 0xDDC, A.stuffit(mac, 2), files, mac=True, faithful=False, exdirs=["Docs"],
        note="a folder in a folder: SparkFS lists it at the top (5.30 too)")
    for m in (0, 1):
        add(f"pit-{'4' if m else 'g'}", 0xDDC, A.packit(mac, m), [(p.split("/")[-1], d) for p, d in files],
            mac=True, refs=["unar"], note="PackIt has no folders")
    # PackIt measures a Huffman-coded fork by where the file pointer got
    # to: Empty's two CRC bytes take under two bytes, its size is negative,
    # and SparkFS's flex_alloc refuses it -- "Not enough free memory", on
    # 5.30 too
    # PackIt both forks: McStuffit gives each file the length 128 + its
    # forks but lays them out padded to 128, so the data fork is cut short;
    # and an empty file's Huffman-coded CRC takes under two bytes, its size
    # is negative, flex_alloc refuses it: "Not enough free memory" -- 5.30
    # does all of this too, and its *Copy then leaves the previous file's
    # first 128 bytes in the new Empty, the box's leaves it empty: compared
    # with 5.30, Empty left out
    add("pit-both", 0xDDC, A.packit(mac, 1), {}, mac=True, mode=3, faithful=False, exdirs=[], ignore=["Empty"],
        note="McStuffitMode 3: SparkFS cuts the data fork short; Empty: its negative size (5.30 too)")
    # Compact Pro: data without its escapes (0x81, 0x82).  A folder's count
    # is of everything in it (unar, macutils); SparkFS's comlist takes it
    # as the folder's own objects (5.30 too), so a folder in a folder takes
    # what follows it: cpt keeps to one level, cpt-nested is compared with
    # 5.30 only.
    cpt_src = [e for e in mac if e[0] not in ("Docs/Random", "Runs")]
    cpt_flat = [(p.replace("Docs/Sub/", "Docs/"), *rest) for p, *rest in cpt_src if p != "Docs/Sub/"]
    add("cpt", 0xDDC, A.compactpro(cpt_flat), files_of(cpt_flat), mac=True, refs=["unar"])
    add("cpt-nested", 0xDDC, A.compactpro(cpt_src), files_of(cpt_src), mac=True, faithful=False,
        refs=["unar"], exdirs=["Docs"], note="a folder in a folder: SparkFS's count differs (5.30 too)")
    for m in (0x82, 0x83, 0xFF):
        add(f"arcfs-{m:x}", 0xDDC, A.arcfs(typed, m), files,
            types={p: t for p, d, t in src if t is not None}, note="no extractor for ArcFS: its LZW is gzip-checked")
    add("packdir", 0x68E, A.packdir(typed), {"Packed/" + p: d for p, d in files},
        types={"Packed/" + p: t for p, d, t in src if t is not None}, note="no extractor for PackDir")

    # CPIO by bsdtar, CAB by gcab: real tools; the tree on disc first, its
    # times the archives' time
    tree = os.path.join(out, "tree")
    if os.path.isdir(tree):
        shutil.rmtree(tree)
    for p, d, *_ in src:
        hp = os.path.join(tree, *p.rstrip("/").split("/"))
        if p.endswith("/"):
            os.makedirs(hp, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(hp), exist_ok=True)
            with open(hp, "wb") as fh:
                fh.write(d)
    for root, ds, fs in os.walk(tree):
        for x in ds + fs:
            os.utime(os.path.join(root, x), (A.TIME, A.TIME))
    names = [p.rstrip("/") for p, d, *_ in src]
    # newc: SparkCPIO's loadheader converted newc's 8 hex digits with
    # strtoul and no terminator, so whatever followed them on the stack
    # counted.  On 5.30, once something has left digits there (an odc
    # archive's 11 octal digits, for one), every newc archive is "Bad
    # archive" until a reset.  The box's build ends the digits
    # (patches/cpio-hex-field.patch): newc is checked against the
    # extractors only, not 5.30.
    for fmt in ("newc", "odc", "bin"):
        tar = shutil.which("bsdtar")
        if not tar:
            log(f"cpio-{fmt}: no bsdtar, skipped")
            continue
        path = os.path.join(out, f"cpio-{fmt},aba")
        subprocess.run([tar, "-cf", path, "--format", "cpio" if fmt == "odc" else fmt, "-n"] + names,
                       cwd=tree, check=True)
        with open(path, "rb") as fh:
            add(f"cpio-{fmt}", 0xABA, fh.read(), files, refs=["7zz"], vs530=(fmt != "newc"),
                note="not compared with 5.30: SparkCPIO's unended hex digits" if fmt == "newc" else "")
    for name in ("cab-store", "cab-mszip"):
        add(name, 0xABF, open(os.path.join(DATA, name + ",abf"), "rb").read(), files,
            faithful=(name != "cab-store"), refs=["cabextract"],
            note="SparkCab reads a stored CAB's data block headers as data (5.30 too)"
            if name == "cab-store" else "")

    nested = os.path.join(ROSGD, "..", "..", "third_party", "SparkFS", "SparkFSApp", "test", "NestedSpark.arc")
    tp = os.environ.get("THIRD_PARTY")
    if tp:
        nested = os.path.join(tp, "SparkFS", "SparkFSApp", "test", "NestedSpark.arc")
    if os.path.exists(nested):
        add("nested", 0xDDC, open(nested, "rb").read(),
            {"Level1.arc/FIle1": b"Hello", "Level1.arc/Folder1/FIle2": b"Hello"}, refs=["unar"],
            exdirs=["Level1.arc", "Level1.arc/Folder1"], note="Spark archives in a Spark archive")
    for t, m in WRITES:
        # --extra: the archives a box's SparkFS wrote (its W directory),
        # read as these are
        p = extra and os.path.join(extra, f"s{t}m{m},ddc")
        if p and os.path.exists(p):
            w = written(t)
            add(f"box-s{t}m{m}", 0xDDC, open(p, "rb").read(), w,
                types=None if t == 3 else {p_: t_ for p_, d, t_ in src if t_ is not None},
                refs=["unar"], exdirs=[] if t == 3 else None, vs530=False, note="written by the box's SparkFS")
    for t, m in WRITES:
        p = os.path.join(DATA, "530", f"s{t}m{m},ddc")
        if os.path.exists(p):
            w = written(t)
            add(f"530-s{t}m{m}", 0xDDC, open(p, "rb").read(), w,
                types=None if t == 3 else {p_: t_ for p_, d, t_ in src if t_ is not None},
                refs=["unar"], exdirs=[] if t == 3 else None,
                note="written by 5.30's SparkFS" + (" (a PK arc: no directories, no RISC OS types)" if t == 3 else ""))
    return res


# ---------------------------------------------------------------- references

def check_refs(arcs, work, log):
    """Each archive through each installed reference extractor: the files
    it gives must be the archive's"""
    bad = 0
    tools = {
        "lha": lambda a, d: ["lha", "xqw=" + d, a],
        "7zz": lambda a, d: ["7zz", "x", "-y", "-o" + d, a],
        "unar": lambda a, d: ["unar", "-q", "-f", "-D", "-o", d, a],
        "cabextract": lambda a, d: ["cabextract", "-q", "-d", d, a],
    }
    for a in arcs:
        for tool in a["refs"]:
            if not shutil.which(tool):
                log(f"refs  {a['name']}: {tool} not installed")
                continue
            d = os.path.join(work, "refs", a["name"], tool)
            if os.path.isdir(d):
                shutil.rmtree(d)
            os.makedirs(d)
            # unar names by extension: give it a copy without RISC OS's
            src = os.path.join(work, "refs", a["name"], "archive" + {
                0xABF: ".cab", 0xABA: ".cpio"}.get(a["type"], ""))
            shutil.copyfile(a["path"], src)
            r = subprocess.run(tools[tool](src, d), capture_output=True, text=True)
            got = {}
            for root, _, fs in os.walk(d):
                for f in fs:
                    rel = os.path.relpath(os.path.join(root, f), d).replace(os.sep, "/")
                    got[rel] = open(os.path.join(root, f), "rb").read()
            want = {k: v for k, v in a["expect"].items()}
            if a["name"] == "nested":
                want = {"Level1:arc/FIle1": b"Hello", "Level1:arc/Folder1/FIle2": b"Hello"}
            if a["name"] == "cab-store" or a["name"] == "cab-mszip":
                got = {k.replace("\\", "/"): v for k, v in got.items()}
            missing = [k for k in want if got.get(k) != want[k]]
            if r.returncode or missing:
                bad += 1
                log(f"refs  {a['name']}: {tool} WRONG: rc {r.returncode}, not as made: {missing[:4]} "
                    f"{r.stderr.strip()[:120]}")
            else:
                log(f"refs  {a['name']}: {tool} gives the {len(want)} files")
    return bad


# ---------------------------------------------------------------- the run

def ro(path):
    """A path in an archive as RISC OS names it: / between parts, . in
    names as /"""
    return ".".join(p.replace(".", "/") for p in path.split("/"))


def dirs_in(a):
    if a.get("exdirs") is not None:
        return a["exdirs"]
    ds = set()
    for p in a["expect"]:
        parts = p.split("/")
        for i in range(1, len(parts)):
            ds.add("/".join(parts[:i]))
    return sorted(ds)


def script(arcs, root):
    """The sections, each (name, Obey lines between its @@ marks): for each
    archive its listing and its copy out, then the write round"""
    P = f"{root}.{SUB}"
    secs = []
    for a in arcs:
        A_ = f"{P}.A.{a['leaf']}"
        L = []
        if a["mac"]:
            # the fork McStuffit gives is chosen as it reads the catalogue:
            # before the archive is first opened
            L.append(f"McStuffitMode {a['mode']}")
        L.append(f"FileInfo {A_}")
        L.append(f"Ex {A_}")
        for d in dirs_in(a):
            L.append(f"Ex {A_}.{ro(d)}")
        L.append(f"CDir {P}.out.{a['leaf']}")
        L.append(f"Copy {A_}.* {P}.out.{a['leaf']}.* ~CFR~V")
        secs.append((a["name"], L))
    for t, m in WRITES if WRITING else []:
        W = f"{P}.W.s{t}m{m}"
        name = f"write-s{t}m{m}"
        if t == 3:
            # a PK arc has no directories: the files, each at the top
            copy = [f"Copy {P}.src.{ro(p)} {W}.{p.split('/')[-1]} ~CFV" for p in written(1)]
            ex = [f"Ex {W}"]
        else:
            copy = [f"Copy {P}.src.* {W}.* ~CFR~V"]
            ex = [f"Ex {W}", f"Ex {W}.Docs"]
        secs.append((name, [f"SparkFSMethod {t} {m}", f"SparkFSCreate {t} {W}"] + copy + ex + [
            f"CDir {P}.out.{name.replace('-', '_')}",
            f"Copy {W}.* {P}.out.{name.replace('-', '_')}.* ~CFR~V"]))
    return secs


def written(t):
    """{path in the archive: bytes} of the write round's archive of type t"""
    src = {p: d for p, d, *_ in source() if not p.endswith("/")}
    return {p.split("/")[-1]: d for p, d in src.items()} if t == 3 else src


def obey_lines(loader, root, secs, last):
    L = list(loader) + [f"CDir {root}.{SUB}.out", f"CDir {root}.{SUB}.W"]
    for name, lines in secs:
        L += [f"Echo @@begin {name}"] + lines + [f"Echo @@end {name}"]
    if last:
        L.append("Echo @@done")
    return L


def run_all(secs, run1, batch, pause, log):
    """Run the sections, `batch` at a time: a run that stops in a section
    (an error, which ends an Obey file) has that section's output so far,
    and the next run starts after it.  The text of every run, joined."""
    text, faults, i, stopped = "", [], 0, {}
    while i < len(secs):
        part = secs[i:i + batch]
        t, f = run1(part, i + len(part) >= len(secs))
        text += t + "\n"
        faults += f
        done, last = sections(t)
        ends = [n for n, _ in part if n in done and f"@@end {n}" in t]
        if len(ends) == len(part):
            i += len(part)
        else:
            stop = last or part[0][0]
            # the error: the last line before the box's power off (QEMU's
            # kernel and rosgd-vz add lines after it), or the farm's last
            lines = t.strip().splitlines()
            off = [i for i, l in enumerate(lines) if l.startswith("rosgd: power off")]
            why = [l for l in (lines[:off[-1]] if off else lines) if l.strip()]
            stopped[stop] = re.sub(r"^rosgd: ", "", why[-1]) if why else "?"
            log(f"run: stopped in {stop}: {stopped[stop]}")
            i = [n for n, _ in secs].index(stop) + 1
        if pause and i < len(secs):
            time.sleep(pause)
    return text, faults, stopped


def stage(share, arcs, app):
    base = os.path.join(share, SUB)
    if os.path.isdir(base):
        shutil.rmtree(base)
    os.makedirs(os.path.join(base, "A"))
    for a in arcs:
        shutil.copyfile(a["path"], os.path.join(base, "A", f"{a['leaf']},{a['type']:03x}"))
    for p, d, t in source():
        hp = os.path.join(base, "src", *p.rstrip("/").split("/"))
        if p.endswith("/"):
            os.makedirs(hp, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(hp), exist_ok=True)
            with open(hp + f",{t:03x}", "wb") as fh:
                fh.write(d)
    if app:
        shutil.copytree(app, os.path.join(base, "!SparkFS"))
    # SparkFSApp's Config.Extensions and Config.NoCo as the commands its
    # !RunImage makes of them (build.sh makes !SparkFS's the same way)
    cfg = os.path.join(DEV_SPARKFS, "Config")
    for leaf, cmd in (("Extensions", "SparkFSExtension"), ("NoCo", "SparkFSNoCo")):
        with open(os.path.join(cfg, leaf), "rb") as fh:
            lines = [l.strip() for l in fh.read().decode("latin-1").replace("\r", "\n").split("\n")]
        with open(os.path.join(base, leaf + ",feb"), "w", encoding="latin-1") as fh:
            fh.write("".join(f"{cmd} {l}\n" for l in lines if l))


def sections(text):
    """{name: [lines]} between the @@ marks; and the last begun"""
    out, cur, last = {}, None, None
    for line in text.replace("\r", "").split("\n"):
        m = re.match(r"^@@(begin|end) (\S+)", line)
        if m:
            if m.group(1) == "begin":
                cur, last = m.group(2), m.group(2)
                out[cur] = []
            else:
                cur = None
            continue
        if cur is not None:
            out[cur].append(line)
    return out, last


LISTING = re.compile(r"^(\S.{11}) (\S+)\s+(\S+)\s+(.*)$")


def listing(lines):
    """The lines of *Ex and *FileInfo that describe objects (not Dir./CSD/
    Lib./URD, not *Copy's); the archive's own *FileInfo (the first) without
    its time and date, which are when it was put on the share"""
    keep = []
    first = True
    for line in lines:
        if first and line.strip():
            first = False
            line = re.sub(r" \d\d:\d\d:\d\d\.\d\d \d\d-\w\w\w-\d{4} ", " ", line)
        s = line.rstrip()
        if not s or s.startswith(("Dir.", "CSD", "Lib.", "URD", "File ", "Created ", "Directory ")) or \
                re.match(r"^\d+ files? copied", s) or re.match(r"^(rosgd|ROSGD)", s) or \
                re.match(r"^\[\s*\d+\.\d+\] ", s):      # the kernel's (the Intel box's console)
            continue
        keep.append(re.sub(r" +", " ", s))
    return keep


def macbinary(b):
    """A file as McStuffit gives both forks (*McStuffitMode 3): a MacBinary
    header (the data fork's length at 83, the resource fork's at 87), then
    -- McStuffit's order; MacBinary's is the other way round -- the
    resource fork, then the data fork, each padded to 128 bytes.  The
    padding is not written for a stored fork read into memory (whatever was
    in SparkFS's buffer is there: 5.30's and the box's differ), so it is
    left out: (header, data, resource)"""
    import struct as st
    if len(b) < 128:
        return b, b"", b""
    dl, rl = st.unpack(">II", b[83:91])
    r0 = 128
    d0 = r0 + ((rl + 127) & ~127)
    return b[:128], b[d0:d0 + dl], b[r0:r0 + rl]


def tree_of(d):
    """{path: (bytes, type)} for a directory on a share, the HostFS ,xxx
    parted from the name (none: Text, &FFF, as HostFS stores it)"""
    got = {}
    for root, _, fs in os.walk(d):
        for f in fs:
            rel = os.path.relpath(os.path.join(root, f), d).replace(os.sep, "/")
            m = re.match(r"^(.*),([0-9a-f]{3})$", rel)
            name, t = (m.group(1), int(m.group(2), 16)) if m else (rel, 0xFFF)
            got[name] = (open(os.path.join(root, f), "rb").read(), t)
    return got


def run_box(share, lines, out, run_script, log):
    import boxrun
    n = len([f for f in os.listdir(out) if f.startswith("serial")])
    cmd = boxrun.obey(share, f"{SUB}/Run", lines)
    path = os.path.join(out, f"serial{n}.log")
    r = boxrun.run(share, cmd, log=path, timeout=900, run_script=run_script)
    log(f"box: {r.status}, {r.elapsed:.1f} s; {path}")
    return r.serial, r.faults


def run_farm(machine, share, lines, out, log):
    path = os.path.join(share, SUB, "Run,feb")
    with open(path, "w", encoding="latin-1") as fh:
        fh.write("\n".join(lines) + "\n")
    env = dict(os.environ)
    farm = env.get("BIGMACFARM", "claude")
    r = subprocess.run([sys.executable, os.path.join(FARM_DIR, "bigmacfarm.py"), "--farm", farm, "run",
                        machine, "--json", "--timeout", "900", f"Obey HostFS:$.{SUB}.Run"],
                       capture_output=True, text=True, env=env)
    try:
        res = json.loads(r.stdout)[0]
    except (ValueError, IndexError):
        log(f"farm: no result: {r.stdout[-300:]} {r.stderr[-300:]}")
        return "", []
    with open(os.path.join(out, "farm.log"), "a") as fh:
        fh.write(res["output"])
    log(f"farm {machine}: {'ok' if res['ok'] else 'FAILED ' + str(res.get('msg'))}, {res['seconds']} s")
    return res["output"] + ("\n" + str(res.get("msg")) if not res["ok"] else ""), []


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out")
    ap.add_argument("--refs", action="store_true", help="check the archives with reference extractors")
    ap.add_argument("--refs-only", action="store_true", help="only that")
    ap.add_argument("--farm", metavar="MACHINE", help="run on RISC OS 5.30 (a bigmacfarm machine)")
    ap.add_argument("--record", action="store_true", help="with --farm: keep 5.30's results in test/data")
    ap.add_argument("--run-script", help="the box's run script")
    ap.add_argument("--rosgd", default=ROSGD)
    ap.add_argument("--extra", metavar="DIR", help="also read the Spark archives a box wrote (its share's SFSTEST.W)")
    ap.add_argument("--only", metavar="PREFIX", help="only the archives (and writes) whose names start so")
    a = ap.parse_args(argv)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.abspath(a.out or os.path.join(ROSGD, "build", "sparkfs-codecs", stamp))
    os.makedirs(out, exist_ok=True)
    results = []

    def log(s):
        print(s, flush=True)

    def probe(name, ok, detail=""):
        results.append((name, bool(ok)))
        print(f"{name:22} {'ok' if ok else 'WRONG'}  {detail}", flush=True)
        return ok

    made = os.path.join(out, "archives")
    os.makedirs(made, exist_ok=True)
    arcs = archives(made, log, a.extra)
    if a.only:
        arcs = [x for x in arcs if x["name"].startswith(a.only)]
        global WRITING
        WRITING = "write".startswith(a.only) or a.only.startswith("write")
    log(f"codecs: {len(arcs)} archives, {len(WRITES) if WRITING else 0} written")
    if a.refs or a.refs_only:
        bad = check_refs(arcs, out, log)
        probe("refs", bad == 0, f"{bad} archive(s) not as an extractor reads them")
        if a.refs_only:
            return 0 if not bad else 1

    if a.farm:
        share = os.path.join(FARM_DIR, "farms", os.environ.get("BIGMACFARM", "claude"), "machines",
                             a.farm, "share")
        stage(share, arcs, DEV_SPARKFS)
        root = "HostFS:$"
        loader = [f"Set SparkFS$Dir {root}.{SUB}.!SparkFS", "SetEval SparkFS$Memory -2",
                  "Set SparkFSRes$Dir <SparkFS$Dir>.Resources.UK", f"Set Spark$Scrap {root}.{SUB}.W",
                  "RMEnsure SparkFS 1.36 RMLoad <SparkFS$Dir>.Resources.SparkFS", "SparkFSImage 1",
                  "SparkFSTruncate 255", "SparkFSBuffers 32", "SparkFSImageTimeout 5", "SparkFSCache 1",
                  "SparkFSMemory 27K 4808K", f"Obey {root}.{SUB}.Extensions", f"Obey {root}.{SUB}.NoCo"] + \
                 [f"RMEnsure {m} 0 RMLoad <SparkFS$Dir>.Modules.{m}" for m in
                  ("Zip", "Tar", "Spark", "Lzh", "ARJ", "Cab", "CPIO", "Zoo", "McStuffit", "PackdDir")]
        secs = script(arcs, root)
        text, faults, stopped = run_all(secs, lambda part, last: run_farm(a.farm, share, obey_lines(loader, root, part, last),
                                                                 out, log), 3, 7, log)
    else:
        share = os.path.join(out, "share")
        if os.path.isdir(share):
            shutil.rmtree(share)
        os.makedirs(share)
        stage(share, arcs, None)
        root = "HostFS::Host.$"
        # SparkFS, its codecs and !SparkFS are the box's ROM's: its Load
        # sets them up, the scrap directory on the share (as the farm's)
        loader = ["Set SparkFS$Dir Resources:$.Apps.!SparkFS", f"Set Spark$Scrap {root}.{SUB}.W",
                  "Obey <SparkFS$Dir>.Load"]
        secs = script(arcs, root)
        rs = a.run_script or os.path.join(os.path.abspath(a.rosgd), "run",
                                          "run-vz.sh" if os.environ.get("ROSGD_ARCH") == "aarch64"
                                          else "run-x86_64.sh")
        # in batches, each boot a fresh SparkFS: with every archive open at
        # once its memory runs out (as 5.30's does), "Not enough free memory"
        text, faults, stopped = run_all(secs, lambda part, last: run_box(
            share, obey_lines(loader, root, part, last), out, rs, log), 8, 0, log)

    secs, last = sections(text)
    base = os.path.join(share, SUB)
    golden = {}
    if os.path.exists(GOLDEN) and not a.farm:
        with open(GOLDEN) as fh:
            golden = json.load(fh)
    record = {}

    for ar in arcs:
        name = ar["name"]
        lines_ = secs.get(name)
        if lines_ is None:
            probe(name, False, "not reached")
            continue
        ls = listing(lines_)
        got = tree_of(os.path.join(base, "out", ar["leaf"]))
        for k in ar["ignore"]:
            got.pop(k, None)
        raw = got
        if ar["mode"] == 3:
            # MacBinary: compared without its padding
            got = {k: (b"".join(macbinary(v[0])), v[1]) for k, v in got.items()}
        hashes = {k: hashlib.sha1(v[0]).hexdigest() for k, v in got.items()}
        record[name] = {"listing": ls, "files": hashes}
        if name in stopped:
            record[name]["error"] = stopped[name]
        isdir = bool(ls) and re.search(r"\bD", ls[0].split()[1] if len(ls[0].split()) > 1 else "")
        problems = []
        if not isdir:
            problems.append(f"not a directory: {ls[:1]}")
        if ar["faithful"]:
            for p, d in ar["expect"].items():
                if p not in got:
                    problems.append(f"{p} missing")
                elif got[p][0] != d:
                    problems.append(f"{p} differs ({len(got[p][0])} bytes, not {len(d)})")
            extra = sorted(set(got) - set(ar["expect"]))
            if extra:
                problems.append(f"extra {extra[:3]}")
        if ar["types"]:
            for p, t in ar["types"].items():
                if p in got and got[p][1] != t:
                    problems.append(f"{p} type {got[p][1]}, not &{t:03X}")
        if ar["forks"]:
            for p, (d, r) in ar["forks"].items():
                if p not in raw:
                    problems.append(f"{p} missing")
                    continue
                h, dd, rr = macbinary(raw[p][0])
                if (dd, rr) != (d, r):
                    problems.append(f"{p}: MacBinary forks {len(dd)}+{len(rr)} bytes, not the archive's")
        if name in stopped:
            ls = [l for l in ls if re.sub(r" +", " ", stopped[name]) != l]
            record[name]["listing"] = ls
        g = golden.get(name) if ar["vs530"] else None
        vs = ""
        if name in stopped and (g is None or g.get("error") != stopped[name]):
            problems.append(f"stopped: {stopped[name]}" + (f" (5.30: {g.get('error')})" if g else ""))
        if g is not None:
            if g["listing"] != ls:
                diff = [x for x in zip(g["listing"], ls) if x[0] != x[1]][:2] or \
                    (g["listing"][len(ls):] or ls[len(g["listing"]):])[:2]
                problems.append(f"listing not 5.30's: {diff}")
            if g["files"] != hashes:
                problems.append(f"files not 5.30's: {sorted(k for k in set(g['files']) | set(hashes) if g['files'].get(k) != hashes.get(k))[:4]}")
            vs = ", as 5.30"
        probe(name, not problems, "; ".join(problems) if problems else
              f"{len(got)} files{vs}"
              f"{' (' + ar['note'] + ')' if ar['note'] else ''}")

    for t, m in WRITES if WRITING else []:
        src = written(t)
        name = f"write-s{t}m{m}"
        lines_ = secs.get(name)
        if lines_ is None:
            probe(name, False, "not reached")
            continue
        got = tree_of(os.path.join(base, "out", name.replace("-", "_")))
        problems = [p for p, d in src.items() if p not in got or got[p][0] != d]
        if name in stopped:
            problems.insert(0, f"stopped: {stopped[name]}")
        arc = os.path.join(base, "W", f"s{t}m{m},ddc")
        detail = f"read back in {'5.30' if a.farm else 'the box'}"
        if shutil.which("unar") and os.path.exists(arc):
            d = os.path.join(out, "unar", name)
            if os.path.isdir(d):
                shutil.rmtree(d)
            shutil.copyfile(arc, os.path.join(out, f"{name}.arc"))
            r = subprocess.run(["unar", "-q", "-f", "-D", "-o", d, os.path.join(out, f"{name}.arc")],
                               capture_output=True, text=True)
            ug = {k.split(",")[0] if re.search(r",[0-9a-f]{3}$", k) else k: v[0] for k, v in tree_of(d).items()}
            bad = [p for p, dd in src.items() if ug.get(p) != dd]
            if r.returncode or bad:
                problems.append(f"unar: rc {r.returncode} {bad[:3]}")
            else:
                detail += ", and by unar"
        keep = os.path.join(DATA, "530", f"s{t}m{m},ddc")
        if a.farm and a.record and os.path.exists(arc) and not os.path.exists(keep):
            # kept once: the 530-* archives (this run listed the ones there)
            os.makedirs(os.path.join(DATA, "530"), exist_ok=True)
            shutil.copyfile(arc, keep)
        probe(name, not problems, f"{'; '.join(problems)}" if problems else detail)

    probe("faults", not faults, faults[0][0] if faults else "no fault report")
    if a.farm and a.record:
        with open(GOLDEN, "w") as fh:
            json.dump(record, fh, indent=1, sort_keys=True)
            fh.write("\n")
        log(f"recorded {GOLDEN}")
    bad = [n for n, good in results if not good]
    print(f"codecs: {len(results) - len(bad)} of {len(results)} as expected; {out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
