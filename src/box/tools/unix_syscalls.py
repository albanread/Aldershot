#!/usr/bin/env python3
"""unix_syscalls.py LINUX_TARBALL OUT_H -- the Unix bridge's call numbers.

Reads arch/x86/entry/syscalls/syscall_64.tbl from a Linux source tarball
(the one the box's kernel is built from, in .cache) and writes a header of
x86-64's system-call numbers, which are the Unix bridge's ABI numbers
(include/rosgd/unixbridge.h) whatever the host is:

    #define UB_<name> <nr>        each "common" and "64" entry
    #define UB_X32_<name> <nr>    each "x32" entry, 512 to 547 (a POSIX
                                  application calls these with the x32 bit)
    UB_NAMES                      name by number, for reports

The host's own numbers (SYS_*) are for making calls, never for decoding the
program's: they differ on an AArch64 box and on a macOS host.
"""
import re
import sys
import tarfile


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    tarball, out = sys.argv[1], sys.argv[2]
    with tarfile.open(tarball) as tf:
        member = next(m for m in tf.getmembers()
                      if m.name.endswith("arch/x86/entry/syscalls/syscall_64.tbl"))
        text = tf.extractfile(member).read().decode()
        version = member.name.split("/")[0]

    common, x32 = {}, {}
    for line in text.splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        nr, abi, name = int(parts[0]), parts[1], parts[2]
        if abi in ("common", "64"):
            common[name] = nr
        elif abi == "x32":
            x32[name] = nr

    lines = [
        "/* SPDX-License-Identifier: MIT",
        " * Copyright (c) 2026 Alban Read */",
        "",
        f"/* ubsys.h -- the Unix bridge's call numbers: x86-64 Linux's, from {version}'s",
        " * arch/x86/entry/syscalls/syscall_64.tbl, by tools/unix_syscalls.py: do not edit.",
        " * UB_X32_* are x32's own entries (the x32 bit is added by the caller). */",
        "#ifndef ROSGD_UBSYS_H",
        "#define ROSGD_UBSYS_H",
        "",
    ]
    for name, nr in sorted(common.items(), key=lambda kv: kv[1]):
        lines.append(f"#define UB_{name} {nr}")
    lines.append("")
    for name, nr in sorted(x32.items(), key=lambda kv: kv[1]):
        lines.append(f"#define UB_X32_{name} {nr}")
    lines.append("")
    top = max(list(common.values()) + list(x32.values())) + 1
    lines.append(f"#define UB_NR_LIMIT {top}")
    lines.append("#define UB_NAMES { \\")
    names = {nr: name for name, nr in common.items()}
    names.update({nr: name for name, nr in x32.items()})
    for nr in range(top):
        if nr in names:
            lines.append(f'    [{nr}] = "{names[nr]}", \\')
    lines.append("}")
    lines.append("")
    lines.append("#endif")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"unix_syscalls: {len(common)} common/64, {len(x32)} x32 -> {out}")


if __name__ == "__main__":
    main()
