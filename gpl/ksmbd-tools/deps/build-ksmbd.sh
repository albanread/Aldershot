#!/bin/sh
# Build ksmbd-tools for the box: the user-space half of ksmbd, Linux's
# in-kernel SMB3 server (CONFIG_SMB_SERVER), which *SMBShare runs -- one
# static program, ksmbd.tools, whose names ksmbd.mountd, ksmbd.adduser,
# ksmbd.addshare and ksmbd.control choose what it does.  Static for x86_64
# or AArch64 Linux (musl), cross-compiled with zig as /init is, over what it needs:
# glib (its glib-2.0 library alone, with PCRE2 from meson's wrap) and
# libnl (libnl-3 and libnl-genl-3).
#
#   sh deps/build-ksmbd.sh [OUT]      OUT defaults to .cache/ksmbd-musl
#
# The sources are .cache/<name>.tar.*, fetched when they are not there and
# checked against SHA-256s: libnl's and glib's as their releases publish
# them, ksmbd-tools's (which publishes none) as first fetched.
set -eu

KSMBD_V=3.5.7
KSMBD_SHA256=d8e532b2d032e3f447f3fd19e0f9ea0abeb355285f8d6871ce9036bc6393a5b2
KSMBD_URL=https://github.com/cifsd-team/ksmbd-tools/releases/download/$KSMBD_V/ksmbd-tools-$KSMBD_V.tar.gz
LIBNL_V=3.12.0
LIBNL_SHA256=fc51ca7196f1a3f5fdf6ffd3864b50f4f9c02333be28be4eeca057e103c0dd18
LIBNL_URL=https://github.com/thom311/libnl/releases/download/libnl3_12_0/libnl-$LIBNL_V.tar.gz
GLIB_V=2.88.3
GLIB_SHA256=ab24d24e698dfa1e408b7bcdb508f4aafc906185a8b8ce72fdf79bbbdc9b383b
GLIB_URL=https://download.gnome.org/sources/glib/2.88/glib-$GLIB_V.tar.xz

HERE=$(cd "$(dirname "$0")/.." && pwd)
CACHE=$HERE/.cache
# TARGET chooses the box (the Makefile's ARCH): x86_64-linux-musl, the
# default, builds under .cache; aarch64-linux-musl under .cache/aarch64
TARGET=${TARGET:-x86_64-linux-musl}
case $TARGET in
    x86_64-linux-musl) TCACHE=$CACHE ;;
    aarch64-linux-musl) TCACHE=$CACHE/aarch64 ;;
    *) echo "$0: TARGET $TARGET: x86_64-linux-musl or aarch64-linux-musl" >&2; exit 2 ;;
esac
TCPU=${TARGET%%-*}
OUT=${1:-$TCACHE/ksmbd-musl}
case $OUT in /*) ;; *) OUT=$HERE/$OUT ;; esac
ZIG=${ZIG:-zig}
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
mkdir -p "$CACHE"

fetch() {   # file url sha256
    if [ ! -f "$CACHE/$1" ]; then
        echo "== fetching $1"
        curl -fsSL -o "$CACHE/$1.part" "$2"
        mv "$CACHE/$1.part" "$CACHE/$1"
    fi
    got=$(shasum -a 256 "$CACHE/$1" 2>/dev/null || sha256sum "$CACHE/$1")
    [ "${got%% *}" = "$3" ] || { echo "$1: SHA-256 mismatch" >&2; exit 1; }
}
fetch ksmbd-tools-$KSMBD_V.tar.gz "$KSMBD_URL" $KSMBD_SHA256
fetch libnl-$LIBNL_V.tar.gz "$LIBNL_URL" $LIBNL_SHA256
fetch glib-$GLIB_V.tar.xz "$GLIB_URL" $GLIB_SHA256

WORK=$TCACHE/ksmbd-build
# meson's downloads for glib's fallbacks (PCRE2 and the like, each checked
# against its wrap's hash) kept with the other sources, so a rebuild needs
# no network
export MESON_PACKAGE_CACHE_DIR=$CACHE/meson-packagecache
mkdir -p "$MESON_PACKAGE_CACHE_DIR"
rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT/lib/pkgconfig" "$OUT/include" "$OUT/bin"
CC="$ZIG cc -target $TARGET"

# ---- libnl: libnl-3 and libnl-genl-3 only (the rest wants flex and bison)
echo "== libnl $LIBNL_V"
tar -xzf "$CACHE/libnl-$LIBNL_V.tar.gz" -C "$WORK"
cd "$WORK/libnl-$LIBNL_V"
CC="$CC" AR="$ZIG ar" RANLIB="$ZIG ranlib" CFLAGS="-O2 -g0" \
    ./configure --host=$TARGET --prefix="$OUT" --disable-shared --enable-static \
    --disable-cli --disable-debug > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$JOBS" lib/libnl-3.la lib/libnl-genl-3.la > build.log 2>&1 || { tail -30 build.log; exit 1; }
cp lib/.libs/libnl-3.a lib/.libs/libnl-genl-3.a "$OUT/lib/"
mkdir -p "$OUT/include/libnl3"
cp -R include/netlink "$OUT/include/libnl3/"
for pc in libnl-3.0 libnl-genl-3.0; do
    sed "s|^prefix=.*|prefix=$OUT|" $pc.pc > "$OUT/lib/pkgconfig/$pc.pc"
done

# ---- glib: the glib-2.0 library, and PCRE2 under it
echo "== glib $GLIB_V"
tar -xJf "$CACHE/glib-$GLIB_V.tar.xz" -C "$WORK"
# meson makes thin archives of a project's own static libraries, which
# zig's linker cannot read: its ar, without the T
cat > "$WORK/ar" <<EOF
#!/bin/sh
op=\$(printf '%s' "\$1" | tr -d T); shift
exec "$ZIG" ar "\$op" "\$@"
EOF
chmod +x "$WORK/ar"
cat > "$WORK/cross.txt" <<EOF
[binaries]
c = ['$ZIG', 'cc', '-target', '$TARGET']
cpp = ['$ZIG', 'c++', '-target', '$TARGET']
ar = '$WORK/ar'
ranlib = ['$ZIG', 'ranlib']
strip = 'true'
pkg-config = 'pkg-config'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = '$TCPU'
cpu = '$TCPU'
endian = 'little'
EOF
cd "$WORK/glib-$GLIB_V"
# the host's own libraries are never the box's
PKG_CONFIG_LIBDIR="$OUT/lib/pkgconfig" PKG_CONFIG_PATH="" \
    meson setup build --cross-file "$WORK/cross.txt" --prefix="$OUT" --libdir=lib \
    --default-library=static --buildtype=release --wrap-mode=forcefallback \
    -Dtests=false -Dintrospection=disabled -Dlibmount=disabled -Dselinux=disabled -Dxattr=false \
    -Dnls=disabled -Dman-pages=disabled -Ddocumentation=false -Dsysprof=disabled -Dlibelf=disabled \
    > meson.log 2>&1 || { tail -40 meson.log; exit 1; }
# the library, and the two a static glib-2.0 links with (a static
# archive's target does not build its dependencies')
PCRE=subprojects/pcre2-10.46/libpcre2-8.a
INTL=subprojects/proxy-libintl-0.5/libintl.a
ninja -C build glib/libglib-2.0.a $PCRE $INTL > build.log 2>&1 || { tail -30 build.log; exit 1; }
cp build/glib/libglib-2.0.a "build/$PCRE" "build/$INTL" "$OUT/lib/"
# installed as glib lays itself out: glib.h and glib-unix.h at the top, the
# rest under glib/, the generated ones beside them, glibconfig.h apart
G=$OUT/include/glib-2.0
mkdir -p "$G/glib" "$OUT/lib/glib-2.0/include"
(cd glib && find . -name '*.h' ! -path './tests/*' ! -name glib.h ! -name glib-unix.h \
    | cpio -pdm --quiet "$G/glib")
cp glib/glib.h glib/glib-unix.h "$G/"
for h in build/glib/*.h; do
    case $h in */glibconfig.h) cp "$h" "$OUT/lib/glib-2.0/include/" ;; *) cp "$h" "$G/glib/" ;; esac
done
cat > "$OUT/lib/pkgconfig/glib-2.0.pc" <<EOF
prefix=$OUT
Name: GLib
Description: the glib-2.0 library alone, static, for ksmbd-tools
Version: $GLIB_V
Libs: -L\${prefix}/lib -lglib-2.0 -lintl -lpcre2-8
Cflags: -I\${prefix}/include/glib-2.0 -I\${prefix}/lib/glib-2.0/include
EOF

# ---- ksmbd-tools
echo "== ksmbd-tools $KSMBD_V"
tar -xzf "$CACHE/ksmbd-tools-$KSMBD_V.tar.gz" -C "$WORK"
cd "$WORK/ksmbd-tools-$KSMBD_V"
PKG_CONFIG_LIBDIR="$OUT/lib/pkgconfig" PKG_CONFIG_PATH="" \
    meson setup build --cross-file "$WORK/cross.txt" --prefix=/usr --sysconfdir=/etc \
    -Drundir=/run --default-library=static --buildtype=release -Dkrb5=disabled \
    -Dc_link_args=-s > meson.log 2>&1 || { tail -40 meson.log; exit 1; }
ninja -C build > build.log 2>&1 || { tail -30 build.log; exit 1; }
cp build/tools/ksmbd.tools "$OUT/bin/"
rm -rf "$WORK"
echo "== $OUT/bin/ksmbd.tools"
