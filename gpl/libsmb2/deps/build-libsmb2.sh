#!/bin/sh
# Build libsmb2, the user-space SMB2/3 client LanManFS uses
# (modules/lanmanfs), for the box: static, for x86_64 or AArch64 Linux
# (musl), cross-compiled with zig as /init is -- or for this machine, for
# the hosted test (TARGET=host).  Over Linux it replaces the kernel's cifs
# mount; the HAL (HybrisOS) has no SMB in its kernel at all.
#
#   sh deps/build-libsmb2.sh [OUT]
#       OUT defaults to .cache/libsmb2-musl (.cache/aarch64/... for
#       aarch64-linux-musl, .cache/libsmb2-host for host); it gets
#       lib/libsmb2.a and include/smb2/
#
# The source is GitHub's archive of Ronnie Sahlberg's repository at a
# pinned commit (master, 8 October 2026: the v6.0.0 tag is from December
# 2024 and lacks two years of fixes), under .cache, fetched when it is not
# there and checked against its SHA-256 either way.  libsmb2 is LGPL 2.1:
# it is linked into /init, and BOX's licence notes say how to relink it.
#
# CMake is not used: the library's core sources (lib/CMakeLists.txt's
# SMB2_CORE_SOURCES: no Kerberos, no Apple CommonCrypto) are compiled
# directly, with deps/libsmb2/config.h in place of CMake's checks.
set -eu

C=fc710a3ebd58a3c15d0ef24322f748e38a3d3a90
SHA=20f9e1489ce60fa1fea5f446b8b7d1204a761b2ccf3baaa810e9e05a0d32e4ce
URL=https://codeload.github.com/sahlberg/libsmb2/tar.gz/$C

HERE=$(cd "$(dirname "$0")/.." && pwd)
CACHE=$HERE/.cache
TARGET=${TARGET:-x86_64-linux-musl}
ZIG=${ZIG:-zig}
case $TARGET in
    x86_64-linux-musl) DEF=$CACHE/libsmb2-musl CC="$ZIG cc -target $TARGET -fPIE" AR="$ZIG ar" ;;
    aarch64-linux-musl) DEF=$CACHE/aarch64/libsmb2-musl CC="$ZIG cc -target $TARGET -fPIE" AR="$ZIG ar" ;;
    host) DEF=$CACHE/libsmb2-host CC=${HOSTCC:-cc} AR=ar ;;
    *) echo "$0: TARGET $TARGET: x86_64-linux-musl, aarch64-linux-musl or host" >&2; exit 2 ;;
esac
OUT=${1:-$DEF}
case $OUT in /*) ;; *) OUT=$HERE/$OUT ;; esac
mkdir -p "$CACHE"

SRC=$CACHE/libsmb2-$C
TGZ=$CACHE/libsmb2-$C.tar.gz
if [ ! -f "$TGZ" ]; then
    echo "== fetching libsmb2 $C"
    curl -fsSL -o "$TGZ.part" "$URL"
    mv "$TGZ.part" "$TGZ"
fi
got=$(shasum -a 256 "$TGZ" 2>/dev/null || sha256sum "$TGZ")
[ "${got%% *}" = "$SHA" ] || { echo "$0: $TGZ: wrong SHA-256" >&2; exit 1; }
if [ ! -f "$SRC/lib/libsmb2.c" ]; then
    tmp=$CACHE/libsmb2-untar.$$
    rm -rf "$tmp" && mkdir -p "$tmp"
    tar xzf "$TGZ" -C "$tmp"
    rm -rf "$SRC"
    mv "$tmp/libsmb2-$C" "$SRC"
    rm -rf "$tmp"
fi

SOURCES="aes aes_reference aes128ccm alloc asn1-ber compat errors hmac hmac-md5 init libsmb2
    md4c md5 ntlmssp pdu sha1 sha224-256 sha384-512 smb2-cmd-cancel smb2-cmd-close
    smb2-cmd-create smb2-cmd-echo smb2-cmd-error smb2-cmd-flush smb2-cmd-ioctl smb2-cmd-lock
    smb2-cmd-logoff smb2-cmd-negotiate smb2-cmd-notify-change smb2-cmd-oplock-break
    smb2-cmd-query-directory smb2-cmd-query-info smb2-cmd-read smb2-cmd-session-setup
    smb2-cmd-set-info smb2-cmd-tree-connect smb2-cmd-tree-disconnect smb2-cmd-write
    smb2-data-file-info smb2-data-filesystem-info smb2-data-security-descriptor
    smb2-data-reparse-point smb2-share-enum smb3-seal smb2-signing socket spnego-wrapper sync
    timestamps unicode usha"

echo "== libsmb2 for $TARGET in $OUT"
rm -rf "$OUT.build"
mkdir -p "$OUT.build"
for f in $SOURCES; do
    $CC -O2 -g -DHAVE_CONFIG_H -D_GNU_SOURCE -I"$HERE/deps/libsmb2" -I"$SRC/include" \
        -I"$SRC/include/smb2" -I"$SRC/lib" -c "$SRC/lib/$f.c" -o "$OUT.build/$f.o"
done
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include/smb2"
$AR rcs "$OUT/lib/libsmb2.a" "$OUT.build"/*.o
cp "$SRC"/include/smb2/*.h "$OUT/include/smb2/"
cp "$SRC/COPYING" "$SRC/LICENCE-LGPL-2.1.txt" "$OUT/"
rm -rf "$OUT.build"
