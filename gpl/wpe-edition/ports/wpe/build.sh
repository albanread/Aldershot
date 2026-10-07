#!/bin/sh
# build.sh -- the WPE edition's parts (design 23, section 10), for either box:
#
#   ARCH=aarch64 sh ports/wpe/build.sh      (default x86_64)
#
# Run from rosgd/.  Makes, under build/wpe (ARCH's):
#   wperun-ARCH        ports/wpe/wperun.c: static, LP64, musl, as *RunBox runs;
#                      it mounts the root and runs a program in it
#   wpe-root-DEB.ext4  the Linux root, Debian's WPE WebKit and what it needs
#                      (ports/wpe/mkwpe.py; DEB is arm64 or amd64), and
#                      ROSGD's compositor if build/wpe/rosgd-compositor-ARCH
#                      is there (ports/compositor/build-box.sh)
# Needs zig (the compiler) and Homebrew's e2fsprogs (mkwpe.py's mke2fs).
set -eu
case "${ARCH:-x86_64}" in
x86_64)  CPU=x86_64; DEB=amd64 ;;
aarch64) CPU=aarch64; DEB=arm64 ;;
*) echo "build.sh: ARCH is x86_64 or aarch64"; exit 1 ;;
esac
W=build/wpe
mkdir -p "$W"
zig cc -target $CPU-linux-musl -static -O2 -Wall -o "$W/wperun-$CPU" ports/wpe/wperun.c
echo "built $W/wperun-$CPU"
# ROSGD's compositor (design 29 G5), when it has been built for this
# architecture (ports/compositor/build-box.sh): at /usr/bin, with wlroots
COMP=
if [ -x "$W/rosgd-compositor-$CPU" ]; then
    COMP="--extra libwlroots-0.20 --add $W/rosgd-compositor-$CPU=/usr/bin/rosgd-compositor"
fi
# and !Browser's engine (ports/browser), built the same way
if [ -x "$W/rosgd-browser-$CPU" ]; then
    COMP="$COMP --add $W/rosgd-browser-$CPU=/usr/bin/rosgd-browser"
fi
python3 ports/wpe/mkwpe.py --arch $DEB --out "$W" $COMP
