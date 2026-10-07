#!/usr/bin/env python3
"""build_musl_a64x32.py OUT RISCOS -- POSIX applications' C library for the
Apple Silicon box: musl for A64X32 (design 22 B8, design 26).

build_musl.py's twin.  x32's musl is zig's bundled copy, built by zig; zig
has no A64X32, so this builds upstream musl 1.2.6 (.cache/musl-1.2.6.tar.gz,
pinned by SHA-256, fetched if missing) with the box's pinned clang
(.cache/llvm: the gnu_ilp32 patch; an unpatched clang compiles
aarch64-linux-gnu_ilp32 with LP64's sizes) and abi/a64x32's target flags,
as musl's own Makefile would.  Writes

    OUT/libc.a      musl with gate.o, switch.o (abi/a64x32/crt) and kernel.o
                    (posix/riscos); musl's maths and string functions are its
                    own C (upstream is complete: nothing is left to zig)
    OUT/crt1.o      _start (abi/a64x32/crt/crt1.c, which is x32's)
    OUT/include/    the headers programs compile against, with RISC OS's
                    <kernel.h> and <swis.h> (swis.h's names from RISCOS)
    OUT/abi         "a64x32": what posix/cc.py and link.py read to compile
                    and link for this machine
                    (no note.o: roscc, which links A64X32, writes the note)

The arch is a64x32 (posix/musl/arch/a64x32): aarch64's types at ILP32's
widths, long double double, and x32's system-call numbers -- they are the
Unix bridge's own numbering (include/rosgd/unixbridge.h), not a kernel's.
Which file builds each object is musl's rule, with these levels, each
replacing the one before for the same directory and name:

    src/X/name.c                    upstream
    src/X/aarch64/name.c            upstream's aarch64 C (the maths); never
                                    its assembler, which takes 64-bit
                                    addresses (memcpy.S, setjmp.s, ...)
    src/X/x32/name.c, X32_SEMANTIC  upstream's x32 C where the calls' x32
                                    meaning is the point: the bridge's
                                    64-bit itimerval, mq_attr
    posix/musl/src/X/x32/name.c     the overlay's calls through the gate
                                    (lseek, clone, vfork, ...), which are
                                    machine-neutral C
    posix/musl/src/X/a64x32/*       A64X32's own: setjmp, longjmp,
                                    sigsetjmp, fenv, dlsym, sysinfo
    posix/musl/src/X/name.c         the overlay's (getenv)
"""
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(HERE)
LLVM = os.environ.get("ROSGD_LLVM", os.path.join(ROSGD, ".cache", "llvm"))
CLANG = os.path.join(LLVM, "bin", "clang")
AR = os.path.join(LLVM, "bin", "llvm-ar")
MUSL = "musl-1.2.6"
MUSL_SHA256 = "d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a"
MUSL_URL = f"https://musl.libc.org/releases/{MUSL}.tar.gz"
ARCH = "a64x32"
# abi/a64x32/flags.mk's target, and long double as double (its table's
# -mlong-double-64); C's division by zero a brk, as in all A64X32 C
TARGET = ["-target", "aarch64-linux-gnu_ilp32", "-fno-pic", "-ffixed-x18", "-fwrapv-pointer",
          "-mno-outline-atomics", "-mlong-double-64",
          "-fsanitize=integer-divide-by-zero", "-fsanitize-trap=integer-divide-by-zero"]
X32_SEMANTIC = {("signal", "getitimer"), ("signal", "setitimer"), ("mq", "mq_open"),
                ("mq", "mq_setattr")}


def fetch(cache):
    tar = os.path.join(cache, MUSL + ".tar.gz")
    if not os.path.exists(tar):
        print(f"build_musl_a64x32: fetching {MUSL_URL}")
        urllib.request.urlretrieve(MUSL_URL, tar + ".part")
        os.replace(tar + ".part", tar)
    with open(tar, "rb") as fh:
        got = hashlib.sha256(fh.read()).hexdigest()
    if got != MUSL_SHA256:
        raise SystemExit(f"build_musl_a64x32: {tar}: SHA-256 {got}, not {MUSL_SHA256}")
    return tar


def sources(musl, overlay):
    """(object name, source) for each object of libc.a, by the levels above"""
    chosen = {}                       # (dir, stem) -> (path, level)

    def add(path, d, level):
        stem = os.path.splitext(os.path.basename(path))[0]
        key = (d, stem)
        if level >= chosen.get(key, (None, -1))[1]:
            chosen[key] = (path, level)

    src = os.path.join(musl, "src")
    for p in sorted(glob.glob(os.path.join(src, "*", "*.c")) +
                    glob.glob(os.path.join(src, "malloc", "mallocng", "*.c"))):
        add(p, os.path.relpath(os.path.dirname(p), src), 0)
    for p in sorted(glob.glob(os.path.join(src, "*", "aarch64", "*.c"))):
        add(p, os.path.basename(os.path.dirname(os.path.dirname(p))), 1)
    for p in sorted(glob.glob(os.path.join(src, "*", "x32", "*.c"))):
        d = os.path.basename(os.path.dirname(os.path.dirname(p)))
        if (d, os.path.splitext(os.path.basename(p))[0]) in X32_SEMANTIC:
            add(p, d, 2)
    osrc = os.path.join(overlay, "src")
    for p in sorted(glob.glob(os.path.join(osrc, "*", "x32", "*.c"))):
        add(p, os.path.basename(os.path.dirname(os.path.dirname(p))), 3)
    for ext in ("c", "s", "S"):
        for p in sorted(glob.glob(os.path.join(osrc, "*", ARCH, "*." + ext))):
            add(p, os.path.basename(os.path.dirname(os.path.dirname(p))), 4)
    for p in sorted(glob.glob(os.path.join(osrc, "*", "*.c"))):
        add(p, os.path.relpath(os.path.dirname(p), osrc), 5)
    return sorted((k[0].replace("/", "_") + "_" + k[1] + ".o", v[0]) for k, v in chosen.items())


def alltypes(arch_in, generic_in):
    """musl's tools/mkalltypes.sed, in Python"""
    out = []
    for line in open(arch_in).read().splitlines() + open(generic_in).read().splitlines():
        m = re.match(r"^TYPEDEF (.*) ([^ ]*);$", line)
        if m:
            t, n = m.groups()
            out += [f"#if defined(__NEED_{n}) && !defined(__DEFINED_{n})", f"typedef {t} {n};",
                    f"#define __DEFINED_{n}", "#endif", ""]
            continue
        m = re.match(r"^(STRUCT|UNION) * ([^ ]*) (.*);$", line)
        if m:
            kind, n, body = m.groups()
            k = kind.lower()
            out += [f"#if defined(__NEED_{k}_{n}) && !defined(__DEFINED_{k}_{n})", f"{k} {n} {body};",
                    f"#define __DEFINED_{k}_{n}", "#endif", ""]
            continue
        out.append(line)
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    out, riscos = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    if not os.path.exists(CLANG):
        raise SystemExit(f"build_musl_a64x32: no {CLANG} (deps/build-llvm.sh: the patched clang)")
    # an unpatched clang takes the triple and compiles LP64 (#38)
    if subprocess.run(["sh", os.path.join(ROSGD, "abi", "a64x32", "check-clang.sh"), CLANG]).returncode:
        raise SystemExit("build_musl_a64x32: only the pinned, patched clang may build A64X32")
    os.makedirs(out, exist_ok=True)
    tar = fetch(os.path.join(ROSGD, ".cache"))
    musl = os.path.join(out, MUSL)
    if not os.path.isdir(musl):
        with tarfile.open(tar) as t:
            t.extractall(out)
    overlay = os.path.join(HERE, "musl")
    obj = os.path.join(out, "obj")
    objdir = os.path.join(obj, "o")
    for d in (objdir, os.path.join(obj, "include", "bits"), os.path.join(obj, "src", "internal")):
        os.makedirs(d, exist_ok=True)

    # musl's generated headers: alltypes.h (a64x32's types), syscall.h
    # (x32's numbers: the bridge's), version.h
    with open(os.path.join(obj, "include", "bits", "alltypes.h"), "w") as fh:
        fh.write(alltypes(os.path.join(overlay, "arch", ARCH, "bits", "alltypes.h.in"),
                          os.path.join(musl, "include", "alltypes.h.in")))
    sysin = open(os.path.join(musl, "arch", "x32", "bits", "syscall.h.in")).read()
    with open(os.path.join(obj, "include", "bits", "syscall.h"), "w") as fh:
        fh.write(sysin + "".join(l.replace("__NR_", "SYS_") + "\n"
                                 for l in sysin.splitlines() if "__NR_" in l))
    with open(os.path.join(obj, "src", "internal", "version.h"), "w") as fh:
        fh.write(f'#define VERSION "{MUSL.split("-")[1]}"\n')

    common = [CLANG] + TARGET + ["-Os", "-ffreestanding", "-nostdinc", "-fno-stack-protector",
                                 "-fomit-frame-pointer", "-fno-unwind-tables",
                                 "-fno-asynchronous-unwind-tables", "-ffunction-sections",
                                 "-fdata-sections", "-frounding-math", "-fno-strict-aliasing",
                                 "-w", "-fno-color-diagnostics"]
    musl_flags = common + ["-std=c99", "-D_XOPEN_SOURCE=700",
                           "-I" + os.path.join(overlay, "arch", ARCH),
                           "-I" + os.path.join(musl, "arch", "aarch64"),
                           "-I" + os.path.join(musl, "arch", "generic"),
                           "-I" + os.path.join(obj, "src", "internal"),
                           "-I" + os.path.join(musl, "src", "include"),
                           "-I" + os.path.join(musl, "src", "internal"),
                           "-I" + os.path.join(obj, "include"),
                           "-I" + os.path.join(musl, "include")]
    jobs = []
    for o, src in sources(musl, overlay):
        flags = list(musl_flags)
        if os.path.basename(os.path.dirname(src)) == "string" and \
                os.path.basename(src).startswith("mem"):
            flags.append("-fno-builtin")       # memset must not become a call to memset
        jobs.append((flags + ["-c", src, "-o", os.path.join(objdir, o)], o))

    # Headers for programs: musl's, generic bits, aarch64's, a64x32's over them
    include = os.path.join(out, "include")
    if os.path.exists(include):
        shutil.rmtree(include)
    shutil.copytree(os.path.join(musl, "include"), include, copy_function=shutil.copyfile,
                    ignore=shutil.ignore_patterns("*.in"))
    for b in (os.path.join(musl, "arch", "generic", "bits"), os.path.join(musl, "arch", "aarch64", "bits"),
              os.path.join(overlay, "arch", ARCH, "bits"), os.path.join(obj, "include", "bits")):
        shutil.copytree(b, os.path.join(include, "bits"), dirs_exist_ok=True,
                        copy_function=shutil.copyfile, ignore=shutil.ignore_patterns("*.in"))
    shutil.copyfile(os.path.join(HERE, "riscos", "kernel.h"), os.path.join(include, "kernel.h"))
    subprocess.run([sys.executable, os.path.join(HERE, "riscos", "swis.py"), riscos,
                    os.path.join(include, "swis.h")], check=True)
    crt_flags = common + ["-std=gnu11", "-I" + os.path.join(ROSGD, "include"), "-I" + include]
    crt = os.path.join(ROSGD, "abi", ARCH, "crt")
    for src in (os.path.join(crt, "gate.c"), os.path.join(crt, "switch.S"), os.path.join(crt, "crt1.c"),
                os.path.join(HERE, "riscos", "kernel.c")):
        o = os.path.splitext(os.path.basename(src))[0] + ".o"
        jobs.append((crt_flags + ["-c", src, "-o", os.path.join(out, o)], o))

    failed = []

    def run(job):
        cmd, o = job
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            failed.append((o, r.stderr.strip()))

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        list(pool.map(run, jobs))
    if failed:
        for o, err in failed[:10]:
            print(f"build_musl_a64x32: {o}:\n{err}", file=sys.stderr)
        raise SystemExit(f"build_musl_a64x32: {len(failed)} files did not compile")

    objs = sorted(os.path.join(objdir, o) for o in os.listdir(objdir))
    objs += [os.path.join(out, o) for o in ("gate.o", "switch.o", "kernel.o")]
    rsp = os.path.join(out, "libc.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(objs) + "\n")
    libc = os.path.join(out, "libc.a")
    if os.path.exists(libc):
        os.remove(libc)
    subprocess.run([AR, "rcs", "--format=gnu", libc, "@" + rsp], check=True)
    with open(os.path.join(out, "abi"), "w") as fh:
        fh.write(ARCH + "\n")
    print(f"build_musl_a64x32: {len(objs)} objects in {libc}")


if __name__ == "__main__":
    main()
