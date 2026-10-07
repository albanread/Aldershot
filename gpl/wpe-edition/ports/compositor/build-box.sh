#!/bin/sh
# build-box.sh -- rosgd-compositor and rosgd-browser (ports/browser) built
# in the box, for the WPE edition's root (ports/wpe/build.sh puts them at
# /usr/bin).
#
#   sh ports/compositor/build-box.sh              the Apple Silicon box's (VZ)
#   ARCH=x86_64 sh ports/compositor/build-box.sh  the Intel box's (QEMU)
#
# (from rosgd/; after make, for that ARCH).  There is no cross-compiler for
# the root's Debian libraries: the development root (mkwpe.py --out
# build/wpe-dev, or build/wpe-dev-amd64; its -dev packages; made here if
# it is not there) has gcc and them, and a headless box builds them in it.
# The share is a scratch directory under build/wpe, removed afterwards.
# Makes build/wpe/rosgd-compositor-CPU and build/wpe/rosgd-browser-CPU.
set -eu
case "${ARCH:-aarch64}" in
aarch64) CPU=aarch64; DEB=arm64; DEV=build/wpe-dev; WPERUN=build/aarch64/wperun ;;
x86_64)  CPU=x86_64; DEB=amd64; DEV=build/wpe-dev-amd64; WPERUN=build/wperun ;;
*) echo "build-box.sh: ARCH is aarch64 or x86_64"; exit 1 ;;
esac
OUT=build/wpe
mkdir -p "$OUT"
if [ ! -f "$DEV/wpe-root-$DEB.ext4" ]; then
    python3 ports/wpe/mkwpe.py --arch $DEB --out "$DEV" --extra busybox build-essential pkgconf \
        libwlroots-0.20-dev wayland-protocols libxkbcommon-dev libpixman-1-dev libwayland-dev \
        libwpewebkit-2.0-dev
fi
SHARE=$(mktemp -d "$PWD/$OUT/compositor-share.XXXXXX")
trap 'rm -rf "$SHARE"' EXIT
mkdir "$SHARE/src" "$SHARE/browser"
cp ports/compositor/rosgd-compositor.c ports/compositor/Makefile "$SHARE/src/"
cp ports/browser/rosgd-browser.c ports/browser/Makefile "$SHARE/browser/"
cat > "$SHARE/build.sh" <<'END'
cd /host/src && make > /host/build.log 2>&1 || echo "build failed" >> /host/build.log
cd /host/browser && make >> /host/build.log 2>&1 || echo "build failed" >> /host/build.log
END
cp "$WPERUN" "$SHARE/wperun,e1f"
RUN='rosgd.run="RunBox HostFS::Host.$.wperun /usr/bin/busybox sh /host/build.sh"'
if [ $CPU = aarch64 ]; then
    ROSGD_DISPLAY=none ROSGD_NET=none ROSGD_SSH=none ROSGD_SHARE="$SHARE" ROSGD_WPE="$PWD/$DEV/wpe-root-$DEB.ext4" \
    ROSGD_APPEND="$RUN" run/run-vz.sh > "$SHARE/serial.log" 2>&1 || true
else
    ROSGD_DISPLAY=none ROSGD_NET=none ROSGD_SSH=none ROSGD_SHARE="$SHARE" ROSGD_WPE="$PWD/$DEV/wpe-root-$DEB.ext4" \
    ROSGD_APPEND="$RUN rosgd.poweroff" run/run-x86_64.sh > "$SHARE/serial.log" 2>&1 < /dev/null || true
fi
if [ ! -x "$SHARE/src/rosgd-compositor" ] || [ ! -x "$SHARE/browser/rosgd-browser" ]; then
    cat "$SHARE/build.log" "$SHARE/serial.log" 2>/dev/null | tail -30
    echo "build-box.sh: not built"
    exit 1
fi
cp "$SHARE/src/rosgd-compositor" "$OUT/rosgd-compositor-$CPU"
cp "$SHARE/browser/rosgd-browser" "$OUT/rosgd-browser-$CPU"
echo "built $OUT/rosgd-compositor-$CPU and $OUT/rosgd-browser-$CPU"
