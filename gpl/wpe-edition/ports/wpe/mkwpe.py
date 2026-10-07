#!/usr/bin/env python3
"""mkwpe.py -- the WPE edition's Linux root: WPE WebKit and everything it
needs, from Debian's packages, as an ext4 image (design 23, W1).

    ports/wpe/mkwpe.py --arch arm64|amd64 [--out DIR] [--suite forky]
                       [--extra PKG ...] [--add FILE=PATH ...] [--list]

The browser runs in the box as an ordinary 64-bit Linux program, chrooted
into this root, which the box mounts read-only at /wpe from a second disk
(a plain ext4 file system labelled "boxwpe").  So it is a small Debian
system, not a port: the packages are Debian's own builds of WPE WebKit
(2.54 in forky, Debian's testing) and its libraries, unchanged.

1. Packages.xz for the suite and architecture, from deb.debian.org.
2. The closure of Depends and Pre-Depends from the roots (ROOTS, and any
   --extra), the first alternative that exists, virtual names through
   Provides.  Versions are not compared: one suite, one version of each.
   Packages that only run maintainer scripts (SKIP) are left out; nothing
   is configured, as no dpkg runs.
3. Each .deb fetched to DIR/debs (kept, so a second run is quick) and
   checked against the index's SHA-256.
4. Their data members merged into one tarball, DIR/wpe-root-ARCH.tar, as
   root-owned files; merged /usr's symlinks (bin, sbin, lib -> usr/...)
   added, and what the packages' maintainer scripts would have made: the
   CA bundle, /etc/passwd and group, nsswitch.conf, the ld.so search path.
5. mke2fs -d makes DIR/wpe-root-ARCH.ext4 from the tarball: the files are
   never unpacked on the Mac, whose file system ignores case.

--list prints the closure and its size and stops after step 2.
"""
import argparse
import hashlib
import io
import lzma
import os
import subprocess
import sys
import tarfile
import time
import urllib.request

MIRROR = "https://deb.debian.org/debian"

# what the browser needs: the engine (its WPEPlatform included), its
# WebDriver (W1's proof that it runs), fonts, certificates and the
# things a page expects of a system
ROOTS = [
    "libwpewebkit-2.0-1",
    "wpewebkit-webdriver",
    "libwpebackend-fdo-1.0-1",      # MiniBrowser (W2's test) still links it
    "libgles2", "libegl1",          # WebKit opens GL ES and EGL at run time;
    "libegl-mesa0",                 # with no GPU, Mesa's software renderer
    "fonts-dejavu-core",
    "fontconfig-config",
    "ca-certificates",
    "shared-mime-info",
    "libc-bin",
]

# packages that are only maintainer scripts, or the tools that run them:
# no dpkg runs in the box, so they would do nothing but take room
SKIP = {
    "debconf", "debconf-2.0", "dpkg", "perl-base", "perl", "adduser",
    "init-system-helpers", "sensible-utils", "install-info", "ucf",
    "base-files", "base-passwd", "bash", "coreutils", "dash", "diffutils",
    "findutils", "grep", "gzip", "hostname", "login", "mawk", "sed", "tar",
    "util-linux", "debianutils", "openssl", "libpam-modules", "libpam0g",
    "libpam-runtime", "passwd", "systemd", "libsystemd-shared",
    "systemd-sysv", "dbus", "dbus-bin", "dbus-daemon", "dbus-system-bus-common",
    "dbus-session-bus-common",
}
# (bubblewrap and xdg-dbus-proxy are kept: WebKit's sandbox, and GTK's image
# loaders', is bubblewrap in the box's namespaces -- design 23 R10)


def fetch(url, dest=None, tries=3):
    for t in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
            if dest:
                tmp = dest + ".part"
                with open(tmp, "wb") as f:
                    f.write(data)
                os.replace(tmp, dest)
            return data
        except Exception as e:  # a mirror hiccup: try again
            if t == tries - 1:
                raise SystemExit(f"mkwpe: {url}: {e}")
            time.sleep(2)


def parse_packages(text):
    pkgs, provides = {}, {}
    for block in text.split("\n\n"):
        f = {}
        key = None
        for line in block.splitlines():
            if line.startswith((" ", "\t")) and key:
                f[key] += "\n" + line
            elif ":" in line:
                key, _, v = line.partition(":")
                f[key] = v.strip()
        if "Package" not in f:
            continue
        pkgs[f["Package"]] = f
        for p in f.get("Provides", "").split(","):
            p = p.strip().split(" ")[0]
            if p:
                provides.setdefault(p, []).append(f["Package"])
    return pkgs, provides


def deps_of(f):
    out = []
    for field in ("Pre-Depends", "Depends"):
        for alt in f.get(field, "").split(","):
            alt = alt.strip()
            if alt:
                out.append([a.strip().split(" ")[0].split(":")[0] for a in alt.split("|")])
    return out


def closure(pkgs, provides, roots):
    want, seen, order = list(roots), set(), []
    skipped = set()
    while want:
        name = want.pop()
        if name in seen:
            continue
        seen.add(name)
        if name in SKIP:
            skipped.add(name)
            continue
        if name not in pkgs:
            raise SystemExit(f"mkwpe: no package {name}")
        order.append(name)
        for alts in deps_of(pkgs[name]):
            pick = None
            for a in alts:
                if a in SKIP:
                    pick = a
                    break
                if a in pkgs:
                    pick = a
                    break
                if a in provides:
                    pick = provides[a][0]
                    break
            if pick is None:
                raise SystemExit(f"mkwpe: {name} needs one of {alts}, none in the suite")
            want.append(pick)
    return sorted(order), sorted(skipped)


def deb_data(path):
    """the data.tar member of a .deb (an ar archive), decompressed"""
    with open(path, "rb") as f:
        d = f.read()
    assert d[:8] == b"!<arch>\n", path
    off = 8
    while off < len(d):
        name = d[off:off + 16].decode().strip().rstrip("/")
        size = int(d[off + 48:off + 58].decode().strip())
        body = d[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name.startswith("data.tar"):
            if name.endswith(".xz"):
                return lzma.decompress(body)
            if name.endswith(".zst"):
                return subprocess.run(["zstd", "-dc"], input=body, capture_output=True, check=True).stdout
            if name.endswith(".gz"):
                import gzip
                return gzip.decompress(body)
            return body
    raise SystemExit(f"mkwpe: {path} has no data.tar")


TRIPLE = {"arm64": "aarch64-linux-gnu", "amd64": "x86_64-linux-gnu"}


def add_file(tar, name, data, mode=0o644):
    ti = tarfile.TarInfo(name)
    ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
    tar.addfile(ti, io.BytesIO(data))


def add_link(tar, name, target):
    ti = tarfile.TarInfo(name)
    ti.type, ti.linkname, ti.mode, ti.mtime = tarfile.SYMTYPE, target, 0o777, int(time.time())
    tar.addfile(ti)


def norm(name):
    """a member name with merged /usr applied: /bin, /sbin, /lib* under /usr"""
    n = name[2:] if name.startswith("./") else name
    for top in ("bin", "sbin", "lib", "lib64"):
        if n == top or n.startswith(top + "/"):
            return "usr/" + n
    return n


def build_tar(arch, order, pkgs, debdir, out, adds=()):
    triple = TRIPLE[arch]
    seen = set()
    certs = []
    with tarfile.open(out, "w", format=tarfile.GNU_FORMAT) as tar:
        for d in ("usr", "usr/bin", "usr/sbin", "usr/lib", "etc", "dev", "proc", "sys", "tmp",
                  "run", "var", "var/tmp", "root", "host", "etc/ssl/certs"):
            ti = tarfile.TarInfo(d)
            ti.type, ti.mode = tarfile.DIRTYPE, 0o1777 if d in ("tmp", "var/tmp") else 0o755
            tar.addfile(ti)
            seen.add(d)
        for top in ("bin", "sbin", "lib"):
            add_link(tar, top, "usr/" + top)
            seen.add(top)
        if arch == "amd64":
            add_link(tar, "lib64", "usr/lib64")
            seen.add("lib64")
        for name in order:
            f = pkgs[name]
            path = os.path.join(debdir, os.path.basename(f["Filename"]))
            with tarfile.open(fileobj=io.BytesIO(deb_data(path))) as src:
                for m in src.getmembers():
                    n = norm(m.name).rstrip("/")
                    if not n or n == "." or n in seen:
                        continue
                    seen.add(n)
                    m.name = n
                    m.uid = m.gid = 0
                    m.uname = m.gname = "root"
                    if m.islnk():
                        m.linkname = norm(m.linkname)
                    if m.isfile():
                        data = src.extractfile(m).read()
                        if n.startswith("usr/share/ca-certificates/") and n.endswith(".crt"):
                            certs.append(data)
                        tar.addfile(m, io.BytesIO(data))
                    else:
                        tar.addfile(m)
        # what maintainer scripts would have made
        add_file(tar, "etc/ssl/certs/ca-certificates.crt", b"".join(c if c.endswith(b"\n") else c + b"\n" for c in certs))
        add_file(tar, "etc/passwd", b"root:x:0:0:root:/root:/bin/sh\n")
        add_file(tar, "etc/group", b"root:x:0:\n")
        add_file(tar, "etc/nsswitch.conf", b"passwd: files\ngroup: files\nhosts: files dns\n")
        add_file(tar, "etc/hosts", b"127.0.0.1 localhost\n::1 localhost\n")
        add_link(tar, "etc/resolv.conf", "../run/resolv.conf")    # wperun copies the box's there
        add_file(tar, "etc/ld.so.conf", f"/usr/local/lib\n/usr/lib/{triple}\n/usr/lib\n".encode())
        add_file(tar, "etc/boxwpe", f"BOX WPE root, Debian forky, {arch}\n".encode())
        # programs of ROSGD's own, built elsewhere (--add): the compositor
        for src, dest in adds:
            add_file(tar, dest.lstrip("/"), open(src, "rb").read(), 0o755)
    return len(certs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", required=True, choices=TRIPLE)
    ap.add_argument("--suite", default="forky")
    ap.add_argument("--out", default=None)
    ap.add_argument("--extra", nargs="*", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--add", action="append", default=[], metavar="FILE=PATH",
                    help="a program put in the root at PATH (ROSGD's compositor, built in the box)")
    a = ap.parse_args()
    adds = []
    for spec in a.add:
        src, _, dest = spec.partition("=")
        if not dest or not os.path.isfile(src):
            raise SystemExit(f"mkwpe: --add {spec}: FILE=PATH, FILE an existing file")
        adds.append((src, dest))
    here = os.path.dirname(os.path.abspath(__file__))
    rosgd = os.path.dirname(os.path.dirname(here))
    out = a.out or os.path.join(rosgd, "build", "wpe")
    debdir = os.path.join(out, "debs", a.arch)
    os.makedirs(debdir, exist_ok=True)

    idx = os.path.join(out, f"Packages-{a.suite}-{a.arch}")
    if not os.path.exists(idx) or time.time() - os.path.getmtime(idx) > 86400:
        raw = fetch(f"{MIRROR}/dists/{a.suite}/main/binary-{a.arch}/Packages.xz")
        with open(idx, "wb") as f:
            f.write(lzma.decompress(raw))
    pkgs, provides = parse_packages(open(idx, encoding="utf-8").read())
    order, skipped = closure(pkgs, provides, ROOTS + a.extra)
    size = sum(int(pkgs[p].get("Installed-Size", "0")) for p in order)
    wpe = pkgs["libwpewebkit-2.0-1"]["Version"]
    print(f"mkwpe: {a.suite} {a.arch}: WPE WebKit {wpe}; {len(order)} packages, "
          f"{size / 1024:.0f} MB installed; left out: {' '.join(skipped)}")
    if a.list:
        for p in sorted(order, key=lambda p: -int(pkgs[p].get("Installed-Size", "0"))):
            print(f"  {int(pkgs[p].get('Installed-Size', '0')) / 1024:8.1f} MB  {p} {pkgs[p]['Version']}")
        return

    for p in order:
        f = pkgs[p]
        path = os.path.join(debdir, os.path.basename(f["Filename"]))
        if not os.path.exists(path):
            fetch(f"{MIRROR}/{f['Filename']}", path)
        h = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if h != f["SHA256"]:
            os.unlink(path)
            raise SystemExit(f"mkwpe: {path}: SHA-256 {h}, the index says {f['SHA256']}")
    print(f"mkwpe: {len(order)} packages fetched and checked")

    tarpath = os.path.join(out, f"wpe-root-{a.arch}.tar")
    ncerts = build_tar(a.arch, order, pkgs, debdir, tarpath, adds)
    tsize = os.path.getsize(tarpath)
    print(f"mkwpe: {tarpath}: {tsize >> 20} MB, {ncerts} CA certificates")

    img = os.path.join(out, f"wpe-root-{a.arch}.ext4")
    make_image(tarpath, img, int(tsize * 1.3 / (1 << 20)) + 64)


def caches(root):
    """What the packages' maintainer scripts would have compiled, where the
    result does not depend on the machine: GLib's settings schemas (without
    them GIO aborts the browser: "No GSettings schemas are installed") and
    the MIME database.  The Mac's own tools make them."""
    schemas = os.path.join(root, "usr/share/glib-2.0/schemas")
    if os.path.isdir(schemas):
        subprocess.run(["glib-compile-schemas", schemas], check=True)
    mime = os.path.join(root, "usr/share/mime")
    if os.path.isdir(mime):
        subprocess.run(["update-mime-database", mime], check=True, capture_output=True)


def make_image(tarpath, img, mb):
    """The ext4 image from the tarball.  Homebrew's mke2fs has no tarball
    input (no libarchive), so the tree is unpacked into a case-sensitive
    APFS disk image -- the Mac's own file system ignores case, and Linux
    trees have names that differ only in it -- and mke2fs -d copies that.
    It copies owners too, the Mac user's, so debugfs then makes every
    inode root's."""
    e2 = os.environ.get("E2FSPROGS", "/opt/homebrew/opt/e2fsprogs/sbin")
    sparse = img + ".sparseimage"
    for p in (img, sparse):
        if os.path.exists(p):
            os.unlink(p)
    subprocess.run(["hdiutil", "create", "-quiet", "-size", f"{mb * 2}m", "-fs", "Case-sensitive APFS",
                    "-volname", "wperoot", "-type", "SPARSE", img], check=True)
    # mounted where hdiutil chooses: a mount point of our own on another
    # volume is refused
    r = subprocess.run(["hdiutil", "attach", "-nobrowse", "-plist", sparse], capture_output=True, check=True)
    import plistlib
    work = [e["mount-point"] for e in plistlib.loads(r.stdout)["system-entities"] if "mount-point" in e][0]
    try:
        root = os.path.join(work, "root")
        os.makedirs(root)
        with tarfile.open(tarpath) as t:
            t.extractall(root, filter="fully_trusted", numeric_owner=True)
        caches(root)
        subprocess.run([f"{e2}/mke2fs", "-q", "-F", "-t", "ext4", "-L", "boxwpe", "-d", root, img, f"{mb}M"],
                       check=True)
        # every inode root's: the paths from the tree, one sif each
        cmds = []
        for dirpath, dirnames, filenames in os.walk(root):
            for n in [""] + dirnames + filenames:
                rel = os.path.relpath(os.path.join(dirpath, n), root)
                rel = "/" if rel == "." else "/" + rel
                if n == "" and dirpath != root:
                    continue
                q = '"' + rel.replace('"', '\\"') + '"'
                cmds.append(f"sif {q} uid 0\nsif {q} gid 0\n")
        script = img + ".debugfs"
        with open(script, "w") as f:
            f.write("".join(cmds))
        r = subprocess.run([f"{e2}/debugfs", "-w", "-f", script, img], capture_output=True, text=True)
        os.unlink(script)
        if r.returncode:
            raise SystemExit(f"mkwpe: debugfs: {r.stderr[-400:]}")
    finally:
        subprocess.run(["hdiutil", "detach", "-quiet", work], check=False)
        os.unlink(sparse)
    subprocess.run([f"{e2}/e2fsck", "-fn", img], check=True, capture_output=True)
    print(f"mkwpe: {img}: {mb} MB, ext4, label boxwpe, checked")


if __name__ == "__main__":
    main()
