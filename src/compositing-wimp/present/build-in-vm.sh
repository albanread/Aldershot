#!/bin/sh
# Build the presenter (design 37, P1) in the Alpine build VM, against the
# same Mesa the present root runs -- run there by vm/buildvm.py --present,
# as root, with rosgd/ at /mnt/rosgd and the build disc at /mnt/build.
#
# The same dev packages mktestroot.sh builds glmark2 with: the root's
# run-time libraries are apk's own (mesa-egl, mesa-gbm, mesa-gles, libdrm).
#
# Output: /mnt/rosgd/.cache/pc/rosgd-present, a dynamic x86_64 musl
# program for the present root (vm/buildvm.py --testroot puts it at
# /usr/bin when it makes the root).
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin    # an ssh shell has no /sbin
ROSGD=/mnt/rosgd
WORK=/mnt/build/present
OUT=$ROSGD/.cache/pc/rosgd-present

apk add -q libdrm-dev mesa-dev

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$ROSGD/ports/present/rosgd-present.c" "$WORK/"
cp "$ROSGD/include/rosgd/screen.h" "$WORK/"
cp "$ROSGD/include/rosgd/presentwire.h" "$WORK/"
# the sprite reader shared with QEMU's metal.m (qemu/ui/box-sprite.c),
# carried in by buildvm.py
cp "$ROSGD/.cache/pc/present-share/box-sprite.c" \
   "$ROSGD/.cache/pc/present-share/box-sprite.h" "$WORK/"
cc -O2 -Wall -Wextra -o "$OUT" "$WORK/rosgd-present.c" "$WORK/box-sprite.c" -I"$WORK" \
   -I/usr/include/libdrm -ldrm -lEGL -lgbm -lGLESv2
rm -rf "$WORK"
ls -l "$OUT"
echo "ROSGD-PRESENT-DONE"
