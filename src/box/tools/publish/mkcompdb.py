#!/usr/bin/env python3
"""mkcompdb.py OUT.json [PATH...] -- a compile_commands.json for the hosted
build's C files, for the analyzers.

The flags are the Makefile's own for hosttest (CFLAGS, HOST_SSL, ASMDEFS and
ASMLIB, read with make -p -n), so the analyzers see each file as the hosted
build compiles it.  The generated headers must be there first:

    make -j8 $(make -p -n build/hosttest | sed -n 's/^GENHDRS := //p')

PATH is a file or a directory of .c files, relative to rosgd/ (default:
runtime platform boot modules, less the vendored code in EXCLUDE).  Run it
from rosgd/."""
import json
import os
import re
import shlex
import subprocess
import sys

DEFAULT = ["runtime", "platform", "boot", "modules"]
# vendored or generated: not ours to analyse or to edit
EXCLUDE = ("modules/picker/", "modules/basicvfp/tier3/", "modules/wimp/")
# files that use Linux's own headers or libsmb2 are compiled as the box's
# /init is, for aarch64 Linux with musl (zig 0.15's headers), not as hosted
GUEST = ("platform/", "boot/main.c", "modules/lanmanfs/")
ZIG = os.path.expanduser("~/zig015/zig")
# generated test headers that nothing in GENHDRS makes: not analysed
SKIP = ("boot/selftest_armrun.c", "boot/selftest_shadow.c", "runtime/armrun/box.c")


def make_vars():
    out = subprocess.run(["make", "-p", "-n", "build/hosttest"], capture_output=True,
                         text=True).stdout
    v = {}
    for name in ("CFLAGS", "HOST_SSL", "ASMDEFS", "ASMLIB"):
        m = re.search(r"^%s :?= (.*)$" % name, out, re.M)
        v[name] = m.group(1) if m else ""
    return v


def expand(s, v):
    # CFLAGS may name other make variables: one pass over the simple ones
    return re.sub(r"\$\((\w+)\)", lambda m: v.get(m.group(1), ""), s)


def guest_flags(flags):
    """The hosted flags with musl's headers and aarch64 Linux as the target."""
    log = subprocess.run([ZIG, "cc", "-target", "aarch64-linux-musl", "-E", "-x", "c", "-", "-v"],
                         input="", capture_output=True, text=True).stderr
    inc = re.search(r"#include <\.\.\.> search starts here:\n(.*?)End of search", log, re.S)
    paths = [l.strip() for l in inc.group(1).splitlines()] if inc else []
    g = [f for f in flags if not f.startswith("-DROS_ARENA_HOSTED")]
    g += ["--target=aarch64-unknown-linux-musl", "-nostdinc"]
    for p in paths:
        g += ["-isystem", p]
    g += ["-I.cache/aarch64/libsmb2-musl/include", "-I.cache/aarch64/openssl-musl/include"]
    return g


def main():
    out = sys.argv[1]
    paths = sys.argv[2:] or DEFAULT
    v = make_vars()
    flags = shlex.split(expand(v["CFLAGS"], v)) + shlex.split(v["HOST_SSL"]) + \
        shlex.split(expand(v["ASMDEFS"], v)) + ["-I" + v["ASMLIB"].replace("-I", "")] \
        + ["-DROS_ARENA_HOSTED"]
    flags = [f for f in flags if f not in ("-O2", "-g")]
    # clang-tidy is Homebrew's, which does not know where the Mac's headers are
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
    if sdk:
        flags += ["-isysroot", sdk]
    root = os.getcwd()
    files = []
    for p in paths:
        if os.path.isfile(p):
            files.append(p)
            continue
        for d, _, fs in os.walk(p):
            files += [os.path.join(d, f) for f in fs if f.endswith(".c")]
    gflags = guest_flags(flags)
    db = []
    for f in sorted(set(files)):
        if f.startswith(EXCLUDE) or "/.cache/" in f or f.startswith("build/") or f in SKIP:
            continue
        use = gflags if f.startswith(GUEST) else flags
        db.append({"directory": root, "file": f,
                   "arguments": ["clang"] + use + ["-c", f]})
    json.dump(db, open(out, "w"), indent=1)
    print("%s: %d files" % (out, len(db)))


main()
