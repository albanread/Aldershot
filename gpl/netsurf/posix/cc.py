#!/usr/bin/env python3
"""cc.py [cc options] -- a C compiler driver for POSIX applications (design 22).

What a Unix package's build expects of `cc`, for the Unix bridge: it
compiles with zig's clang for x32 against ROSGD's musl, and links with
posix/link.py -- an image the C applications' loader runs.  Ports use it as
CC (posix.mk's POSIX_CC_DRIVER; `sh posix/cc` is the same).

    compile   -c, -S, -E, -M*: zig clang -target x86_64-linux-muslx32
              -fno-pic -nostdinc -isystem $ROSGD_POSIX/include
              -fno-stack-protector, then the options as given
    link      everything else: objects and archives in order, -L dirs and
              -lNAME resolved to libNAME.a (static only), passed to link.py;
              options that only make sense for a dynamic or a GNU link
              (-shared, -rdynamic, -Wl,--trace, -pthread, -lm, -lc, ...)
              are dropped, and -static is what it always is
    queries   --version, -dumpmachine, -print-*: zig clang's answers,
              with the machine x86_64-linux-muslx32

$ROSGD_POSIX is posix-libc's output (build/gen/posix by default); $ZIG
names zig; $ROSGD_CFLAGS_EXTRA is added to every compile; $ROSGD_LIBPATH
(directories, colon-separated) is searched for -lNAME after the link's -L.

For the Apple Silicon box (design 26) $ROSGD_POSIX is build_musl_a64x32.py's
output (build/aarch64/gen/posix), whose file "abi" says a64x32: then the
compiler is the box's pinned clang (.cache/llvm, $ROSGD_LLVM) with
A64X32's target flags (abi/a64x32/flags.mk's, long double double), the
machine aarch64-linux-gnu_ilp32, and link.py links with roscc.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ZIG = os.environ.get("ZIG", "zig")
POSIX = os.environ.get("ROSGD_POSIX", os.path.join(os.path.dirname(HERE), "build", "gen", "posix"))

def _abi():
    try:
        with open(os.path.join(POSIX, "abi")) as fh:
            return fh.read().strip()
    except OSError:
        return "x32"


ABI = _abi()
if ABI == "a64x32":
    LLVM = os.environ.get("ROSGD_LLVM", os.path.join(os.path.dirname(HERE), ".cache", "llvm"))
    CC = [os.path.join(LLVM, "bin", "clang")]
    MACHINE = "aarch64-linux-gnu_ilp32"
    TARGET = ["-target", MACHINE, "-fno-pic", "-ffixed-x18", "-fwrapv-pointer",
              "-mno-outline-atomics", "-mlong-double-64",
              "-fsanitize=integer-divide-by-zero", "-fsanitize-trap=integer-divide-by-zero",
              "-nostdinc", "-isystem", os.path.join(POSIX, "include"), "-fno-stack-protector",
              "-fno-asynchronous-unwind-tables"]
else:
    CC = [ZIG, "clang"]
    MACHINE = "x86_64-linux-muslx32"
    TARGET = ["-target", "x86_64-linux-muslx32", "-fno-pic", "-nostdinc",
              "-isystem", os.path.join(POSIX, "include"), "-fno-stack-protector"]
# Libraries every Unix link names that are part of libc here, or mean nothing
IN_LIBC = {"m", "c", "pthread", "rt", "dl", "util", "crypt", "resolv", "xnet"}
DROP = {"-shared", "-rdynamic", "-pthread", "-static", "-static-libgcc", "-pie", "-no-pie",
        "-s", "-fPIC", "-fpic", "-fPIE", "-fpie"}


def clang(args):
    # $ROSGD_CFLAGS_EXTRA: options a port adds to every compile, after its own
    extra = os.environ.get("ROSGD_CFLAGS_EXTRA", "").split()
    os.execvp(CC[0], CC + TARGET + args + extra)


def link(args):
    out, inputs, libdirs, ld_opts = "a.out", [], [], []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            out = args[i + 1]
            i += 2
        elif a.startswith("-L"):
            libdirs.append(a[2:] or args[i + 1])
            i += 1 if len(a) > 2 else 2
        elif a.startswith("-l"):
            name = a[2:] or args[i + 1]
            i += 1 if len(a) > 2 else 2
            if name in IN_LIBC:
                continue
            path = [d for d in os.environ.get("ROSGD_LIBPATH", "").split(":") if d]
            for d in libdirs + path + [os.path.join(POSIX, "lib")]:
                p = os.path.join(d, f"lib{name}.a")
                if os.path.exists(p):
                    inputs.append(p)
                    break
            else:
                raise SystemExit(f"cc.py: no static lib{name}.a in {libdirs}")
        elif a.startswith("-Wl,"):
            # passed on to ld.lld, but not the GNU-only ones
            ld_opts += [o for o in a[4:].split(",") if o and o not in GNU_ONLY]
            i += 1
        elif a.startswith("-"):
            # compile-time options, meaningless to a link: skip, with an argument if they take one
            i += 2 if a in ("-include", "-isystem", "-I", "-D", "-U", "-MF", "-MT", "-MQ", "-x") else 1
        elif a.endswith((".c", ".s", ".S")):
            obj = out + "." + os.path.basename(a) + ".o"
            subprocess.run(CC + TARGET + ["-c", a, "-o", obj], check=True)
            inputs.append(obj)
            i += 1
        else:
            inputs.append(a)                    # an object or an archive
            i += 1
    cmd = [sys.executable, os.path.join(HERE, "link.py"), "--musl", POSIX, "-o", out]
    for o in ld_opts:
        cmd.append("--ld-opt=" + o)
    os.execv(sys.executable, cmd + inputs)


GNU_ONLY = {"--trace", "-E", "--as-needed", "--no-as-needed", "--export-dynamic",
            "--start-group", "--end-group"}
COMPILE_DROP = {"-fPIC", "-fpic", "-fPIE", "-fpie", "-pthread"}


def main():
    args = sys.argv[1:]
    if "-dumpmachine" in args:
        print(MACHINE)
        return
    # A query only when nothing is to be compiled or linked: a build's "-v"
    # beside its inputs (CMake's ABI probe) is just verbosity
    inputs = [a for a in args if not a.startswith("-") and a.endswith((".c", ".s", ".S", ".o", ".a"))]
    if not args or (not inputs and any(a in ("--version", "-v", "-dumpversion") or
                                       a.startswith("-print-") for a in args)):
        clang(args)
    args = [a for a in args if a != "-v"]
    if any(a in ("-c", "-S", "-E", "-M", "-MM") for a in args):
        clang([a for a in args if a not in COMPILE_DROP])
    link([a for a in args if a not in DROP])


if __name__ == "__main__":
    main()
