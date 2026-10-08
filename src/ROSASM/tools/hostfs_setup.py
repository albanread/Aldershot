#!/usr/bin/env python3
"""Lay out RPCEmu's HostFS directory from the RISC OS distribution archives.

    hostfs_setup.py [--dest DIR] [--dry-run] [archive.zip ...]

The emulator serves `hostfs/` to RISC OS as `HostFS::HostFS.$`, and the
differential harness assembles on the far side of it: it needs a bootable
RISC OS and the DDE's ObjAsm sitting in there.

A plain unzip will not do, because a RISC OS file carries a filetype and a
plain extract drops it. Without the filetype `!Boot` is not an Obey file and
`objasm` is not a program, so nothing runs.

## What the archives carry

Every entry has an `AC` extra field (id 0x4341), twenty bytes: the signature
`ARC0`, then the load and exec addresses and the attributes. The filetype
lives in the load address, which is how RISC OS has always carried it:

    load & 0xFFF00000 == 0xFFF00000  ->  filetype = (load >> 8) & 0xFFF

and anything else is a raw load/exec pair, for a file that is loaded at an
address rather than typed.

## What HostFS expects on the host

From `hostfs.c`, which is the authority here:

* a typed file is `leafname,xxx`, the filetype in lowercase hex;
* an untyped one is `leafname,load-exec`, both in lowercase hex;
* a name with no comma reads back as text, 0xFFF.

The path mapping is already done by the archives. RISC OS separates
directories with `.` and allows `/` inside a name, so `$.AcornC/C++.!SetPaths`
is stored as `AcornC.C++/!SetPaths` -- which is exactly HostFS's own swap of
`.` and `/`. The names come out of the zip usable as they stand, and only the
suffix has to be added.
"""
import argparse
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402

DEST = paths.HOSTFS
ARCHIVES = [
    # The boot disc. Its contents go at the root, so the wrapper is stripped.
    (os.path.join(paths.ROMS, "HardDisc4.5.30.zip"), "HardDisc4"),
    # The DDE, which brings AcornC/C++ and with it ObjAsm.
    (os.path.join(paths.DEVROOT, "ROOL_DDE30-9TFC.zip"), None),
]

ACORN_EXTRA_ID = 0x4341  # 'AC'
DEFAULT_FILE_TYPE = 0xFFF  # Text


def acorn_fields(info):
    """The load and exec addresses from an entry's `AC` extra field."""
    extra = info.extra
    i = 0
    while i + 4 <= len(extra):
        ident, size = struct.unpack_from("<HH", extra, i)
        body = extra[i + 4 : i + 4 + size]
        if ident == ACORN_EXTRA_ID and len(body) >= 12 and body[:4] == b"ARC0":
            load, exec_ = struct.unpack_from("<II", body, 4)
            return load, exec_
        i += 4 + size
    return None, None


def suffix_for(load, exec_):
    """The comma suffix HostFS reads the filetype back out of."""
    if load is None:
        return ""
    if (load & 0xFFF0_0000) == 0xFFF0_0000:
        return ",%03x" % ((load >> 8) & 0xFFF)
    return ",%x-%x" % (load, exec_)


def host_name(name, load, exec_):
    """The host path for an archive entry, suffix and all.

    Only `?` needs translating: the archives already store the RISC OS `.`/`/`
    swap that HostFS does, and `?` is `#` on the host because a question mark
    is a wildcard in a RISC OS pathname.
    """
    parts = [p.replace("?", "#") for p in name.split("/") if p]
    if not parts:
        return None
    parts[-1] += suffix_for(load, exec_)
    return os.path.join(*parts)


def extract(archive, strip, dest, dry_run=False, log=None):
    """Unpack one archive into `dest`, keeping filetypes."""
    written = skipped = 0
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename
            if strip:
                if not name.startswith(strip + "/"):
                    continue
                name = name[len(strip) + 1 :]
            if not name:
                continue
            if info.is_dir():
                if not dry_run:
                    os.makedirs(os.path.join(dest, host_name(name, None, None) or ""),
                                exist_ok=True)
                continue
            load, exec_ = acorn_fields(info)
            rel = host_name(name, load, exec_)
            if rel is None:
                continue
            out = os.path.join(dest, rel)
            # First archive wins: the boot disc is laid down before the DDE,
            # and where both carry a name the disc's is the one that boots.
            if os.path.exists(out):
                skipped += 1
                continue
            if dry_run:
                written += 1
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                dst.write(src.read())
            written += 1
    if log is not None:
        log.append(f"{os.path.basename(archive)}: {written} files, {skipped} already there")
    return written, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive", nargs="*", help="zip files; the defaults if none")
    ap.add_argument("--dest", default=DEST)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    todo = [(p, None) for p in a.archive] or ARCHIVES
    for path, _ in todo:
        if not os.path.isfile(path):
            raise SystemExit(f"{path}: not there")

    if not a.dry_run:
        os.makedirs(a.dest, exist_ok=True)
    log = []
    for path, strip in todo:
        extract(path, strip, a.dest, a.dry_run, log)
    for line in log:
        print(line)

    if not a.dry_run:
        n = sum(len(f) for _, _, f in os.walk(a.dest))
        print(f"{a.dest}: {n} files")
        # The harness runs ObjAsm by a short path, because a RISC OS command
        # line is truncated at 256 bytes.
        src = os.path.join(a.dest, "AcornC.C++", "!SetPaths", "Lib32", "objasm,ff8")
        for cand in (src, src[: -len(",ff8")]):
            if os.path.isfile(cand):
                short = os.path.join(a.dest, "oa" + cand[len(src) - 4 :])
                with open(cand, "rb") as f, open(short, "wb") as g:
                    g.write(f.read())
                print(f"objasm also at {os.path.basename(short)} for short commands")
                break
        else:
            print("objasm not found in the DDE archive", file=sys.stderr)


if __name__ == "__main__":
    main()
