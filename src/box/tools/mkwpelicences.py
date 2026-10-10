#!/usr/bin/env python3
"""mkwpelicences.py OUT -- the licences of the WPE edition's Linux root.

    python3 tools/mkwpelicences.py OUT      (run from rosgd/, after
                                            ports/wpe/build.sh for each ARCH)

The root (ports/wpe/mkwpe.py) is Debian's own packages, unchanged, plus
ROSGD's compositor and !Browser's engine.  For each root built under
build/wpe (wpe-root-arm64.tar, wpe-root-amd64.tar) this writes:

    OUT/packages-ARCH.txt     every package in it: name, version, and the
                              Debian source package it is built from (the
                              source, at that version, is in Debian's archive:
                              https://snapshot.debian.org/package/SOURCE/VERSION/)
    OUT/copyright/PKG.txt     each package's licence as Debian ships it
                              (/usr/share/doc/PKG/copyright in the root),
                              once for both architectures

The package list is mkwpe.py's own closure, read from the Packages index the
root was built from, so it is the list of what the image holds.
"""
import importlib.util
import os
import sys
import tarfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("mkwpe", os.path.join(HERE, "ports", "wpe", "mkwpe.py"))
mkwpe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkwpe)

# what the editions are built with (ports/wpe/build.sh, ports/compositor/build-box.sh)
EXTRA = ["libwlroots-0.20"]


def closure(arch, suite="forky"):
    idx = os.path.join(HERE, "build", "wpe", f"Packages-{suite}-{arch}")
    pkgs, provides = mkwpe.parse_packages(open(idx, encoding="utf-8").read())
    order, _ = mkwpe.closure(pkgs, provides, mkwpe.ROOTS + EXTRA)
    return pkgs, order


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(os.path.join(out, "copyright"), exist_ok=True)
    written = set()
    for arch in ("arm64", "amd64"):
        tarpath = os.path.join(HERE, "build", "wpe", f"wpe-root-{arch}.tar")
        if not os.path.exists(tarpath):
            print(f"mkwpelicences: no {tarpath} (ARCH=... sh ports/wpe/build.sh): {arch} left out")
            continue
        pkgs, order = closure(arch)
        lines = [f"The WPE edition's Linux root, {arch}: {len(order)} Debian packages (forky), unchanged.",
                 "Each is built from the Debian source package named; its source, at that version, is in",
                 "Debian's archive: https://snapshot.debian.org/package/<source>/<version>/",
                 "Each package's licence is copyright/<package>.txt beside this file.",
                 "", f"{'package':40} {'version':32} source",
                 f"{'-' * 40} {'-' * 32} {'-' * 30}"]
        for p in sorted(order):
            f = pkgs[p]
            src = f.get("Source", p)
            if " (" in src:                              # "src (version)": a binNMU
                src, sver = src.split(" (", 1)
                src = f"{src} {sver.rstrip(')')}"
            else:
                src = f"{src} {f['Version']}"
            lines.append(f"{p:40} {f['Version']:32} {src}")
        with open(os.path.join(out, f"packages-{arch}.txt"), "w") as fh:
            fh.write("\n".join(lines) + "\n")
        # the copyright files, as the root holds them (a doc directory that is
        # a link to another package's is that package's licence)
        with tarfile.open(tarpath) as t:
            for m in t.getmembers():
                parts = m.name.split("/")
                if len(parts) == 5 and parts[:3] == ["usr", "share", "doc"] and parts[4] == "copyright" \
                        and m.isfile() and parts[3] not in written:
                    data = t.extractfile(m).read()
                    with open(os.path.join(out, "copyright", parts[3] + ".txt"), "wb") as fh:
                        fh.write(data)
                    written.add(parts[3])
        print(f"mkwpelicences: {arch}: {len(order)} packages")
    print(f"mkwpelicences: {len(written)} copyright files in {out}/copyright")


if __name__ == "__main__":
    main()
