# ports/wpe -- the WPE edition's browser engine

WPE WebKit, the real web, for the WPE edition of BOX (RISCOSGrandDesign
design 23, section 10). It runs in the box as ordinary 64-bit Linux
programs, in a small Debian system of their own: Debian's builds of WPE
WebKit and the libraries it needs, unchanged, on a second disk that the
box mounts read-only at `/wpe`.

| File | What it is |
|---|---|
| `mkwpe.py` | builds that root as an ext4 image, from Debian's packages |
| `wperun.c` | mounts the root and runs a program in it (static, for `*RunBox`) |
| `build.sh` | both, for `ARCH` |
| `wdcheck.py` | W2's check: a page loaded and drawn in the box, through WebDriver |

## Building

    ARCH=aarch64 sh ports/wpe/build.sh       the Apple silicon box's (arm64)
    sh ports/wpe/build.sh                    the Intel box's (amd64)

It needs zig and Homebrew's e2fsprogs and shared-mime-info. Output is in
`build/wpe`:
- `wperun-<cpu>`;
- `wpe-root-<deb arch>.ext4`, about 830 MB, with WPE WebKit 2.54.0 from
  Debian forky;
- the packages, kept in `debs/`.

`mkwpe.py` does five things:
1. Resolves the dependency closure of its roots from forky's package index.
2. Fetches each package and checks it against the index's SHA-256.
3. Merges the packages into one tarball, with merged `/usr`.
4. Adds what the packages' maintainer scripts would have made: the CA
   bundle, `/etc/passwd`, the compiled GSettings schemas and the MIME
   database. No dpkg runs in the box.
5. Makes the image. The tree is unpacked on a case-sensitive APFS disk
   image, because the Mac's own file system ignores case. `mke2fs -d`
   copies it, and `debugfs` makes every file root's.

## Running

`ROSGD_WPE=<image>` attaches the image to a box as a second, read-only disk.
`run/run-vz.sh` and `run/run-qemu.sh` both take it, and the box then gets
4 GB. Then, in the box:

    *RunBox wperun /usr/bin/WPEWebDriver --port=4444 --host=all

The first time, `wperun` sets up the root:
- it finds the disk labelled `boxwpe` and mounts it at `/wpe`;
- it gives it `/dev`, `/proc`, `/sys` and the share at `/host`;
- it mounts tmpfs at `/tmp`, `/run`, `/var/tmp`, `/root` and `/dev/shm`;
- it copies the box's `resolv.conf` into it.

Every time, it changes root to `/wpe` and runs the program. Three things
are deliberate:
- **No GPU.** An empty `/dev/dri` hides the display device, which RISC OS
  owns. WPE then renders with Skia, through Mesa's software renderer,
  into shared memory. That is the CPU-first path of design 23's D3.
- **No WebKit sandbox.** WebKit's sandbox is bubblewrap, which needs user
  namespaces, and the box's kernel has none.
- **A writable `/etc/resolv.conf`.** It is a link to `/run/resolv.conf`,
  because the root is read-only.

`ports/wpe/wdcheck.py <box address> <url> <out.png>` asks that WebDriver to
start MiniBrowser headless, load the page and send back its screenshot. On
6 October 2026, in the Apple silicon box under VZ, https://example.com and
Wikipedia's "RISC OS" article both came back drawn in full, over HTTPS.
