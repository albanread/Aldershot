#!/usr/bin/env python3
"""build_musl.py OUT RISCOS -- the C library of POSIX applications (design 22).

zig's bundled musl (1.2.5), built for x32 as musl's own Makefile builds it,
with ROSGD's overlay (posix/musl) and start files (abi/x32/crt).  Writes

    OUT/libc.a      musl, with gate.o and switch.o (abi/x32/crt): its system
                    calls go through the Unix gate; and what zig's copy of
                    musl leaves to zig (lib/c's string functions,
                    compiler_rt's mem* and maths), built from zig's sources
    OUT/crt1.o      _start: the loader's entry, UNIX_ROSGD_INIT, argv, main
    OUT/note.o      the ROSGD note the loader requires
    OUT/include/    the headers programs compile against: musl's, and the
                    generated bits for x32; and RISC OS's <kernel.h> and
                    <swis.h> (posix/riscos, U-g), whose functions are in
                    libc.a too.  swis.h's SWI names come from the OS's
                    headers in RISCOS, a source drop's RiscOS directory
                    (posix/riscos/swis.py)

Which file builds each object is musl's rule: src/X/name.c, unless
src/X/x32/name.{c,s,S} replaces it; and the overlay's file of the same
directory and name replaces either.  $ZIG names zig (default: zig).
"""
import concurrent.futures
import glob
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(HERE)
ZIG = os.environ.get("ZIG", "zig")
ARCH = "x32"
OTHER_ARCHES = {"a64x32"}


def zig_lib_dir():
    out = subprocess.run([ZIG, "env"], capture_output=True, text=True, check=True).stdout
    try:
        return json.loads(out)["lib_dir"]
    except json.JSONDecodeError:
        # zig 0.15 prints a ZON-like record: .lib_dir = "...",
        for line in out.splitlines():
            if ".lib_dir" in line:
                return line.split('"')[1]
    raise SystemExit("build_musl: cannot find zig's lib directory")


def sources(musl, overlay):
    """(object name, source) for each object of libc.a, by musl's rule"""
    chosen = {}                       # (dir, stem) -> path

    def add(path, root, arch_level):
        rel = os.path.relpath(path, root)
        parts = rel.split(os.sep)
        stem = os.path.splitext(parts[-1])[0]
        d = parts[:-1]
        if d and d[-1] == ARCH:
            d = d[:-1]
        key = ("/".join(d), stem)
        level = chosen.get(key, (None, -1))[1]
        if arch_level >= level:
            chosen[key] = (path, arch_level)

    for p in sorted(glob.glob(os.path.join(musl, "src", "*", "*.c")) +
                    glob.glob(os.path.join(musl, "src", "malloc", "mallocng", "*.c"))):
        add(p, musl, 0)
    for ext in ("c", "s", "S"):
        for p in sorted(glob.glob(os.path.join(musl, "src", "*", ARCH, "*." + ext))):
            add(p, musl, 1)
    for p in sorted(glob.glob(os.path.join(overlay, "src", "**", "*.[csS]"), recursive=True)):
        if os.path.basename(os.path.dirname(p)) not in (ARCH, "") and \
                os.path.basename(os.path.dirname(p)) in OTHER_ARCHES:
            continue                    # another machine's (build_musl_a64x32.py's)
        add(p, overlay, 2)
    return sorted((k[0].replace("/", "_") + "_" + k[1] + ".o", v[0]) for k, v in chosen.items())


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    out, riscos = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    lib = zig_lib_dir()
    musl = os.path.join(lib, "libc", "musl")
    bits = os.path.join(lib, "libc", "include", "x86_64-linux-musl" + ARCH)
    overlay = os.path.join(HERE, "musl")
    objdir = os.path.join(out, "obj")
    os.makedirs(objdir, exist_ok=True)

    common = [ZIG, "clang", "-target", "x86_64-linux-muslx32", "-fno-pic", "-Os",
              "-ffreestanding", "-nostdinc", "-fno-stack-protector", "-fomit-frame-pointer",
              "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-ffunction-sections",
              "-fdata-sections", "-frounding-math", "-fno-strict-aliasing", "-w",
              "-Wa,--noexecstack", "-fno-color-diagnostics"]
    musl_flags = common + ["-std=c99", "-D_XOPEN_SOURCE=700",
                           "-I" + os.path.join(overlay, "arch", ARCH),
                           "-I" + os.path.join(musl, "arch", ARCH),
                           "-I" + os.path.join(musl, "arch", "generic"),
                           "-I" + os.path.join(musl, "src", "include"),
                           "-I" + os.path.join(musl, "src", "internal"),
                           "-I" + bits,
                           "-I" + os.path.join(musl, "include")]

    jobs = []
    for obj, src in sources(musl, overlay):
        flags = list(musl_flags)
        if os.path.basename(os.path.dirname(src)) == "string" and \
                os.path.basename(src).startswith("mem"):
            flags.append("-fno-builtin")       # memset must not become a call to memset
        jobs.append((flags + ["-c", src, "-o", os.path.join(objdir, obj)], obj))

    # Headers for programs: musl's own, with the arch's bits and the generated
    # x32 ones over them -- what the start files compile against too
    include = os.path.join(out, "include")
    if os.path.exists(include):
        shutil.rmtree(include)
    shutil.copytree(os.path.join(musl, "include"), include, copy_function=shutil.copyfile)
    for b in (os.path.join(musl, "arch", "generic", "bits"), os.path.join(musl, "arch", ARCH, "bits"),
              os.path.join(bits, "bits")):
        shutil.copytree(b, os.path.join(include, "bits"), dirs_exist_ok=True,
                        copy_function=shutil.copyfile)
    shutil.copyfile(os.path.join(HERE, "riscos", "kernel.h"), os.path.join(include, "kernel.h"))
    subprocess.run([sys.executable, os.path.join(HERE, "riscos", "swis.py"), riscos,
                    os.path.join(include, "swis.h")], check=True)
    crt_flags = common + ["-std=gnu11", "-I" + os.path.join(ROSGD, "include"), "-I" + include]
    crt = os.path.join(ROSGD, "abi", ARCH, "crt")          # the start files are the ABI's
    for src in (os.path.join(crt, "gate.c"), os.path.join(crt, "switch.s"), os.path.join(crt, "crt1.c"),
                os.path.join(crt, "note.s"), os.path.join(HERE, "riscos", "kernel.c")):
        obj = os.path.splitext(os.path.basename(src))[0] + ".o"
        jobs.append((crt_flags + ["-c", src, "-o",
                                  os.path.join(out, obj)], obj))

    failed = []

    def run(job):
        cmd, obj = job
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            failed.append((obj, r.stderr.strip()))

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        list(pool.map(run, jobs))
    if failed:
        for obj, err in failed[:10]:
            print(f"build_musl: {obj}:\n{err}", file=sys.stderr)
        raise SystemExit(f"build_musl: {len(failed)} files did not compile")

    # What zig's copy of musl leaves out because zig supplies it: the string
    # functions of lib/c (strlen, strcmp, strcasecmp, ...) and compiler_rt's
    # (memcpy, memmove, sin, cos, floor, complex multiplication, ...), built
    # for x32 from zig's own sources
    for name, src in (("compiler_rt", "compiler_rt.zig"), ("zigc", "c.zig")):
        r = subprocess.run([ZIG, "build-obj", "-target", "x86_64-linux-muslx32", "-O",
                            "ReleaseSmall", "-fno-PIC", "-fno-stack-check", "--name", name,
                            os.path.join(lib, src)], cwd=out, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"build_musl: {src}:\n{r.stderr}")

    objs = sorted(os.path.join(objdir, o) for o in os.listdir(objdir))
    objs += [os.path.join(out, o) for o in ("gate.o", "switch.o", "kernel.o", "compiler_rt.o", "zigc.o")]
    rsp = os.path.join(out, "libc.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(objs) + "\n")
    libc = os.path.join(out, "libc.a")
    if os.path.exists(libc):
        os.remove(libc)
    subprocess.run([ZIG, "ar", "rcs", "--format=gnu", libc, "@" + rsp], check=True)

    print(f"build_musl: {len(objs)} objects in {libc}")


if __name__ == "__main__":
    main()
