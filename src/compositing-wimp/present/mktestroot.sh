#!/bin/sh
# The GPU test root (RISCOSGrandDesign design 37, P0): Alpine's own Mesa,
# libdrm and the GL test tools, as an ext4 file system.  It is built in the
# build VM (vm/buildvm.py --testroot), which is Alpine itself, so the
# packages are the VM's own apk's -- the same libraries P5's own Mesa build
# replaces later.
#
# The file system is labelled boxwpe, which is what wperun (in the ROM)
# mounts and runs programs from, so on the box, over SSH:
#
#   *RunBox /usr/bin/wperun /usr/bin/eglinfo -B
#   *RunBox /usr/bin/wperun /usr/bin/kmscube
#   *RunBox /usr/bin/wperun /usr/bin/glmark2-es2-drm
#
# glmark2 has no Alpine package: it is built from its release (2023.01,
# SHA-256 below, cached in .cache) with the drm-glesv2 flavour, which is
# glmark2-es2-drm, EGL on GBM with no window system -- the shape of the
# presenter itself.  It is built in the VM against the same Mesa version
# (apk's) and installed into the root by DESTDIR, so the root carries no
# toolchain.
#
# Output: /mnt/rosgd/.cache/pc/testroot.ext4.  Not reproducible byte for
# byte: apk's packages move under a pinned version.  It is a test tool,
# not a release artefact.
set -eu
ROSGD=/mnt/rosgd
ROOT=/mnt/build/testroot
OUT=$ROSGD/.cache/pc/testroot.ext4
GLMARK2=glmark2-2023.01
GLMARK2_SHA=8fece3fc323b643644a525be163dc4931a4189971eda1de8ad4c1712c5db3d67

# what the tests run on and with; mesa-demos is eglinfo, es2_info and the
# rest, kmscube proves EGL on GBM with a KMS page flip, mesa-dri-gallium is
# the drivers (nouveau for the T1000, vmwgfx's svga for VMware, iris for
# Intel), mesa-vulkan-nouveau is NVK -- since Mesa 25.1, GL on Turing goes
# through Zink on NVK by default, so Vulkan is part of the GL path itself,
# and vulkan-tools' vulkaninfo says which driver actually ran.
# alpine-keys: apk checks the packages' signatures with the keys in the
# root it installs into.
PACKS="alpine-baselayout alpine-keys busybox mesa-dri-gallium mesa-egl
       mesa-gles mesa-gbm mesa-gl libdrm mesa-demos kmscube
       mesa-vulkan-nouveau vulkan-tools eudev-libs libjpeg-turbo libpng"

echo "== the test root: apk $PACKS"
rm -rf "$ROOT"
mkdir -p "$ROOT"
# --root starts with no repositories and no keys of its own, so both are
# named: the VM is the same Alpine, and its keys sign these packages
REPO=https://dl-cdn.alpinelinux.org/alpine/v3.24
apk --root "$ROOT" --keys-dir /etc/apk/keys --cache-dir /mnt/build/apkcache \
    --repository "$REPO/main" --repository "$REPO/community" \
    --initdb --no-progress add $PACKS
[ -f "$ROOT/bin/busybox" ] ||
    { echo "ROSGD-TESTROOT-FAILED: busybox did not install"; exit 1; }
nouveau=$(find "$ROOT/usr/lib" -name 'nouveau*so*' 2>/dev/null | head -1)
[ -n "$nouveau" ] ||
    { echo "ROSGD-TESTROOT-FAILED: no nouveau driver in the test root"; exit 1; }
echo "== the drivers include $(basename "$nouveau")"

echo "== eglinfo and kmscube: checking"
[ -x "$ROOT/usr/bin/eglinfo" ] ||
    { echo "ROSGD-TESTROOT-FAILED: no eglinfo (mesa-demos)"; ls "$ROOT/usr/bin"; exit 1; }
[ -x "$ROOT/usr/bin/kmscube" ] ||
    { echo "ROSGD-TESTROOT-FAILED: no kmscube"; exit 1; }

echo "== $GLMARK2 from source (drm-glesv2)"
apk add -q curl    # the live VM has none, and the build tools come later
tarball=$ROSGD/.cache/$GLMARK2.tar.gz
if ! echo "$GLMARK2_SHA  $tarball" | sha256sum -c - >/dev/null 2>&1; then
    curl -fsSL -o "$tarball" "https://codeload.github.com/glmark2/glmark2/tar.gz/refs/tags/${GLMARK2#glmark2-}" ||
        { echo "ROSGD-TESTROOT-FAILED: $GLMARK2.tar.gz is not there"; exit 1; }
    echo "$GLMARK2_SHA  $tarball" | sha256sum -c - >/dev/null ||
        { echo "ROSGD-TESTROOT-FAILED: $GLMARK2.tar.gz is not the one pinned"; exit 1; }
fi
# built in the VM, against apk's own Mesa (the same version the root has),
# installed into the root only as files.  eudev-dev: the drm flavour's
# device discovery is libudev's (eudev-libs is the root's run-time half).
apk add -q meson samurai libdrm-dev mesa-dev eudev-dev libjpeg-turbo-dev libpng-dev
B=/mnt/build/$GLMARK2
rm -rf "$B"
mkdir -p "$B"
tar -xzf "$tarball" -C "$B" --strip-components=1
meson setup "$B/build" "$B" -Dflavors=drm-glesv2 --prefix=/usr
ninja -C "$B/build"
DESTDIR="$ROOT" ninja -C "$B/build" install
rm -rf "$B"
[ -x "$ROOT/usr/bin/glmark2-es2-drm" ] ||
    { echo "ROSGD-TESTROOT-FAILED: no glmark2-es2-drm"; exit 1; }

# p0report: what design 37's P0 asks the card, in one run over SSH:
#   *RunBox /usr/bin/wperun /usr/bin/p0report        Zink on NVK (the default)
#   *RunBox /usr/bin/wperun /usr/bin/p0report nvc0   the classic nvc0 driver
# The argument, not an environment variable, because wperun execs programs
# with an environment of its own.  Everything is written to /tmp as well as
# the terminal, so the report survives a hung test (it is in RAM until the
# box restarts).  wperun sets LIBGL_ALWAYS_SOFTWARE=1 for the browser's
# sake; a GL test must not inherit it, or every figure measures the CPU.
mkdir -p "$ROOT/usr/bin"
cat > "$ROOT/usr/bin/p0report" <<'REPORT'
#!/bin/sh
# design 37, P0.  Modes: the loader's default (Zink on NVK, on Turing) and,
# with the argument nvc0, the classic nouveau driver: loader.c chooses with
# debug_get_bool_option("NOUVEAU_USE_ZINK", ...), so 0 asks for nvc0.
mode=${1:-zink}
[ "$mode" = nvc0 ] && export NOUVEAU_USE_ZINK=0
unset LIBGL_ALWAYS_SOFTWARE
# wperun's PATH has no /bin, where busybox's commands are
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
report=/tmp/p0report-$mode.txt
failed=/tmp/p0report-$mode.failed
rm -f "$failed"

report() {
    echo
    echo "=== $*"
}

{
    echo "p0report: mode $mode, NOUVEAU_USE_ZINK=${NOUVEAU_USE_ZINK-unset}, $(date)"
    report "the kernel's side (dmesg)"
    dmesg | grep -iE 'nouveau|firmware|gsp'
    report "the kernel's side (DRM clients)"
    mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
    for d in /sys/kernel/debug/dri/*; do
        [ -d "$d" ] || continue
        echo "-- $d/clients:"
        cat "$d/clients" 2>/dev/null
    done
    report "eglinfo"
    eglinfo -B 2>&1
    soft=0
    eglinfo -B 2>&1 | grep -qiE 'llvmpipe|softpipe' && soft=1
    [ "$soft" = 0 ] || {
        echo
        echo "P0: SOFTWARE RENDERING -- the GL figures below are the processor's, not the card's." >&2
    }
    report "vulkaninfo"
    vulkaninfo --summary 2>&1
    report "the card's clocks"
    for h in /sys/class/hwmon/hwmon*; do
        [ -r "$h/name" ] || continue
        n=$(cat "$h/name")
        case "$n" in nouveau*) ;; *) continue ;; esac
        echo "-- $n ($h):"
        for f in "$h"/freq*_input "$h"/power1_input; do
            [ -r "$f" ] && echo "  $(basename "$f") = $(cat "$f")"
        done
    done
    for d in /sys/kernel/debug/dri/*; do
        [ -r "$d/pstate" ] && { echo "-- $d/pstate:"; cat "$d/pstate"; }
    done
    report "kmscube (10 s: proof, not a benchmark)"
    timeout 10 kmscube 2>&1
    report "glmark2-es2-drm (every scene)"
    glmark2-es2-drm 2>&1
    [ "$soft" = 0 ] || : > "$failed"
} 2>&1 | tee "$report"
[ -e "$failed" ] && exit 1
echo
echo "p0report: the report is $report as well"
REPORT
chmod 755 "$ROOT/usr/bin/p0report"

# the presenter (P1), when it has been built (vm/buildvm.py --present):
# at /usr/bin, where /init will start it
if [ -s "$ROSGD/.cache/pc/rosgd-present" ]; then
    cp "$ROSGD/.cache/pc/rosgd-present" "$ROOT/usr/bin/rosgd-present"
    chmod 755 "$ROOT/usr/bin/rosgd-present"
    echo "== the root carries /usr/bin/rosgd-present"
fi
# /host: wperun binds the box's disc there (the presenter's updated
# program is dropped on it, design 37's live updates) -- and the root is
# read-only once mounted, so the directory is baked in here
mkdir -p "$ROOT/host"

echo "== the file system"
mkdir -p "$ROSGD/.cache/pc"
MB=$(( $(du -sm "$ROOT" | cut -f1) * 2 + 16 ))
mke2fs -q -F -t ext4 -L boxwpe -E root_owner=0:0 -d "$ROOT" "$OUT" "${MB}M"
ls -l "$OUT"
echo "ROSGD-TESTROOT-DONE"
