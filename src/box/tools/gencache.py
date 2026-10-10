#!/usr/bin/env python3
"""gencache.py --dir CACHE --gen GEN --out OUT.c [--in FILE]... -- CMD... [';;' CMD...]
-- run a translation (rosasm, through build.py or rom_unit.py), or take
its result from a cache when everything it reads is unchanged.

A ROM unit's C (OUT.c, and OUT.h beside it when the command writes one)
depends only on what the command reads: the Makefile's prerequisites
(--in), the rosasm binary and the Python tools named on its command line
(and the tools beside them, which they import), the SWI tables in GEN
(native_swis.txt, swi_regs.txt), ROM_UNIT_SOURCE and ROM_UNIT_EXTRA, and
the RISC OS sources, a fixed drop named by its path.  The key is a hash
of all of that: each file by its contents, every other argument as
written, with GEN and OUT made placeholders, so any build tree -- the
shared checkout, a scratch tree, another session's -- finds what another
translated.  rosasm's output names no path of the tree that made it.

The cache is a directory of <key>/ holding the outputs; a result is
written to a temporary directory and renamed into place, so make -j and
other builds at the same time see a whole entry or none.  Nothing is ever
evicted: an entry is ~3-11 MB, and the directory can be removed at any
time (ROSGD_GENCACHE=off, or GENCACHE= to make, turns it off).

Commands are separated by the argument ';;' and run in turn, each needing
success (BASICVFP's build.py, then patch-basicasm.py on its output).

    gencache.py --object --dir CACHE --out OBJ -- CC FLAGS... -c -o OBJ SRC

compiles a generated unit's C (the Wimp's is 5.5 MB and a minute of one
core), or takes the object from the cache.  The key is the compiler's
--version, its arguments (this tree's path made a placeholder) and the
source as its preprocessor gives it; that same run writes the object's
dependency file, so make tracks the headers either way.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True             # no __pycache__ in tools/

VERSION = b"gencache 1\n"


def file_hash(path, seen):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def key_of(gen, out, inputs, commands):
    gen = os.path.abspath(gen)
    out = os.path.abspath(out)
    k = hashlib.sha256(VERSION)
    seen = set()

    def add_file(tag, path):
        p = os.path.abspath(path)
        if p in seen:
            return
        seen.add(p)
        k.update(f"{tag} {norm(path)} {file_hash(p, seen)}\n".encode())
        if p.endswith(".py"):               # the modules a tool imports
            d = os.path.dirname(p)
            for n in sorted(os.listdir(d)):
                q = os.path.join(d, n)
                if n.endswith(".py") and q not in seen and os.path.isfile(q):
                    seen.add(q)
                    k.update(f"lib {n} {file_hash(q, seen)}\n".encode())

    def norm(s):
        a = os.path.abspath(s) if os.path.exists(s) else s
        if a == out:
            return "$OUT"
        if a.startswith(gen + os.sep) or a == gen:
            return "$GEN" + a[len(gen):]
        if os.path.exists(s):
            return os.path.basename(a) if os.path.isfile(a) else a
        return s

    for f in sorted(inputs):
        if os.path.isfile(f):
            add_file("in", f)
    for n in ("native_swis.txt", "swi_regs.txt"):
        if os.path.isfile(os.path.join(gen, n)):
            add_file("gen", os.path.join(gen, n))
    for name in ("ROM_UNIT_SOURCE", "ROM_UNIT_EXTRA"):
        v = os.environ.get(name, "")
        k.update(f"env {name}={norm(v) if v else ''}\n".encode())
        if v and os.path.isfile(v):
            add_file("env", v)
    for cmd in commands:
        k.update(b"cmd\n")
        for a in cmd:
            if os.path.isfile(a) and os.path.abspath(a) != out:
                add_file("arg", a)
            else:
                k.update(f"arg {norm(a)}\n".encode())
    return k.hexdigest()


def outputs(out):
    return [out, os.path.splitext(out)[0] + ".h"]


def object_key(cmd, out):
    """An object's key: the compiler's version, its flags (this tree's
    paths made placeholders) and the source as the preprocessor gives it --
    which also writes the object's dependency file (-MF, -MT), so make
    knows its headers whether the object is compiled or taken"""
    root = os.getcwd()
    i = next((n for n, a in enumerate(cmd) if a.startswith("-")), len(cmd))
    compiler = cmd[:i]
    k = hashlib.sha256(VERSION + b"object\n")
    v = subprocess.run(compiler + ["--version"], capture_output=True)
    k.update(v.stdout)
    pre, skip = [], False
    dep, mp = os.path.splitext(out)[0] + ".d", False
    for n, a in enumerate(cmd):
        if skip:
            skip = False
            continue
        if a == "-c":
            pre.append("-E")
        elif a in ("-o", "-MF", "-MT"):
            if a == "-MF":
                dep = cmd[n + 1]            # the rule's own name for it
            skip = True
        elif a in ("-MMD", "-MD"):
            pass
        elif a == "-MP":
            mp = True
        else:
            pre.append(a)
    pre += ["-MMD"] + (["-MP"] if mp else []) + ["-MF", dep, "-MT", out, "-o", "-"]
    r = subprocess.run(pre, capture_output=True)
    if r.returncode:
        sys.stderr.buffer.write(r.stderr)
        sys.exit(r.returncode)
    text = r.stdout.replace(root.encode(), b"$ROOT")
    for a in cmd[i:]:
        if a == out:
            a = "$OUT"
        k.update(a.replace(root, "$ROOT").encode() + b"\n")
    k.update(text)
    return k.hexdigest()


def cached_object(cache, cmd, out):
    """--object: compile, or take the object from the cache"""
    key = object_key(cmd, out)
    entry = os.path.join(cache, "obj-" + key)
    name = os.path.basename(out)
    if os.path.isfile(entry):
        tmp = out + ".gencache"
        shutil.copyfile(entry, tmp)
        os.replace(tmp, out)
        print(f"gencache: {name} from the cache ({key[:12]})")
        return 0
    r = subprocess.run(cmd)
    if r.returncode:
        return r.returncode
    os.makedirs(cache, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".tmp-", dir=cache)
    os.close(fd)
    shutil.copyfile(out, tmp)
    os.replace(tmp, entry)
    print(f"gencache: {name} compiled, cached ({key[:12]})")
    return 0


def main(argv):
    if "--" not in argv:
        raise SystemExit(__doc__)
    i = argv.index("--")
    opts, rest = argv[:i], argv[i + 1:]
    cache = gen = out = None
    inputs = []
    obj = False
    j = 0
    while j < len(opts):
        o = opts[j]
        if o == "--object":
            obj = True
            j += 1
        elif o in ("--dir", "--gen", "--out", "--in") and j + 1 < len(opts):
            v = opts[j + 1]
            if o == "--dir":
                cache = v
            elif o == "--gen":
                gen = v
            elif o == "--out":
                out = v
            else:
                inputs += v.split()
            j += 2
        else:
            raise SystemExit(f"gencache: unknown option {o}\n\n{__doc__}")
    if obj:
        if not (out and rest):
            raise SystemExit(__doc__)
        if not cache or os.environ.get("ROSGD_GENCACHE") == "off":
            return subprocess.run(rest).returncode
        return cached_object(cache, rest, out)
    if not (gen and out and rest):
        raise SystemExit(__doc__)
    commands, cur = [], []
    for a in rest:
        if a == ";;":
            commands.append(cur)
            cur = []
        else:
            cur.append(a)
    commands.append(cur)
    commands = [c for c in commands if c]

    def run():
        for c in commands:
            r = subprocess.run(c)
            if r.returncode:
                sys.exit(r.returncode)

    off = not cache or os.environ.get("ROSGD_GENCACHE") == "off"
    if off:
        run()
        return 0
    key = key_of(gen, out, inputs, commands)
    entry = os.path.join(cache, key)
    name = os.path.basename(out)
    if os.path.isdir(entry):
        for src_leaf, dest in zip((name, os.path.splitext(name)[0] + ".h"), outputs(out)):
            src = os.path.join(entry, src_leaf)
            if os.path.isfile(src):
                tmp = dest + ".gencache"
                shutil.copyfile(src, tmp)
                os.replace(tmp, dest)
        print(f"gencache: {name} from the cache ({key[:12]})")
        return 0
    run()
    os.makedirs(cache, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix=".tmp-", dir=cache)
    try:
        for src in outputs(out):
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(tmpdir, os.path.basename(src)))
        try:
            os.rename(tmpdir, entry)
        except OSError:                     # another build stored it first
            shutil.rmtree(tmpdir, ignore_errors=True)
    except BaseException:
        shutil.rmtree(tmpdir, ignore_errors=True)
        raise
    print(f"gencache: {name} translated, cached ({key[:12]})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
