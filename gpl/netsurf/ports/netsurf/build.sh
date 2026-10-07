#!/bin/bash
# build.sh [deps|libs|monkey|httpd|rosgd|all] -- NetSurf as a POSIX
# application (design 22, section 11): its libraries and the image
# libraries, built for x32 against ROSGD's musl with posix/cc, and NetSurf's
# monkey front end linked over them -- the Unix half proved before the new
# RISC OS front end is written.  No curl or OpenSSL: NetSurf's http and https
# go through RISC OS's own URL_Fetcher and AcornHTTP (curl on a worker
# thread), by ROSGD's fetcher (frontend/fetch_url.c), so transfers carry on
# while NetSurf is paged out.  monkey is linked with that fetcher and
# monkey/harness.c; httpd (monkey/httpd.c) is the Linux server the box's
# checks fetch from.  rosgd is the Wimp front end (frontend/, TARGET=rosgd),
# made an application: !NetSurf.
#
# Run from rosgd/ after make posix-libc.  Inputs:
#   $NETSURF   the NetSurf clones (default third_party/NetSurf beside the
#              toolchain: this tree's ../../third_party, else the main
#              worktree's, as riscos-src is found; THIRD_PARTY names it)
#   $SRC       zlib, libpng, libjpeg-turbo, expat, utf8proc and bison tarballs
#              (default deps-macos-intel/src beside the toolchain, found so)
#   $ARCH      x86_64 (the Intel box, the default) or aarch64 (the Apple
#              Silicon box, design 26): A64X32 against make ARCH=aarch64
#              posix-libc's musl (posix/build_musl_a64x32.py), linked by
#              roscc; everything then under build/aarch64, the libraries in
#              a64x32/ in place of x32/
# Output: build/ports/netsurf (W): x32/ the libraries, host/ nsgenbind and
# bison for the Mac, ws/netsurf/nsmonkey the program, httpd the test server,
# !NetSurf the application.
set -euo pipefail

ROSGD=$(pwd)
# beside the toolchain: this tree's, else the main worktree's (#34)
beside() {
    local main d
    main=$(git worktree list --porcelain 2>/dev/null | sed -n '1s/^worktree //p')
    for d in "$ROSGD/../../$1" "$main/../$1"; do
        [ -d "$d" ] && { (cd "$d" && pwd); return; }
    done
    echo "$ROSGD/../../$1"
}
NETSURF=${NETSURF:-${THIRD_PARTY:-$(beside third_party)}/NetSurf}
SRC=${SRC:-$(beside deps-macos-intel/src)}
case "${ARCH:-x86_64}" in
x86_64)  B=build; LIBS=x32; TRIPLE=x86_64-linux-muslx32; CPU=x86_64; LINUX=x86_64-linux-musl ;;
aarch64) B=build/aarch64; LIBS=a64x32; TRIPLE=aarch64-linux-gnu_ilp32; CPU=aarch64
         LINUX=aarch64-linux-musl; PNGOPT=--disable-hardware-optimizations ;;
*) echo "build.sh: ARCH is x86_64 or aarch64"; exit 1 ;;
esac
W=$ROSGD/$B/ports/netsurf
X=$W/$LIBS
HOSTP=$W/host
export ROSGD_POSIX=$ROSGD/$B/gen/posix
export ROSGD_LIBPATH=$X/lib
CC=$ROSGD/posix/cc
export AR=$ROSGD/posix/ar RANLIB=$ROSGD/posix/ranlib
export PKG_CONFIG_LIBDIR=$X/lib/pkgconfig:$X/share/pkgconfig
export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR
export PATH=$HOSTP/bin:$PATH
J=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
mkdir -p "$W/deps" "$X/lib/pkgconfig" "$X/include" "$HOSTP"

[ -f "$ROSGD_POSIX/libc.a" ] || { echo "build.sh: make ARCH=${ARCH:-x86_64} posix-libc first"; exit 1; }

deps() {
    cd "$W/deps"
    if [ ! -x "$HOSTP/bin/bison" ]; then
        echo "=== bison 3 (the Mac's, for nsgenbind and libnslog)"
        tar xf "$SRC"/bison-3.8.2.tar.xz
        (cd bison-3.8.2 && ./configure --prefix="$HOSTP" >/dev/null && make -j"$J" >/dev/null &&
         make install >/dev/null)
    fi
    echo "=== zlib"
    rm -rf zlib-1.3.2 && tar xf "$SRC"/zlib-1.3.2.tar.xz
    (cd zlib-1.3.2 && CHOST=$TRIPLE CC=$CC ./configure --static --prefix="$X" >/dev/null &&
     make -j"$J" libz.a >/dev/null && make install >/dev/null)
    echo "=== libpng"
    rm -rf libpng-1.6.50 && tar xf "$SRC"/libpng-1.6.50.tar.xz
    # no NEON on A64X32: arm_neon.h is not among the box's headers
    (cd libpng-1.6.50 && ./configure --host=$TRIPLE CC=$CC --prefix="$X" --disable-shared \
       ${PNGOPT:-} \
       --enable-static CPPFLAGS=-I"$X"/include LDFLAGS=-L"$X"/lib >/dev/null &&
     make -j"$J" libpng16.la >/dev/null && make install-libLTLIBRARIES install-pkgincludeHEADERS \
       install-nodist_pkgincludeHEADERS install-pkgconfigDATA install-header-links \
       install-library-links install-libpng-pc >/dev/null)
    echo "=== libjpeg-turbo"
    rm -rf libjpeg-turbo-3.2.0 jpeg-build && tar xf "$SRC"/libjpeg-turbo-3.2.0.tar.gz
    cmake -S libjpeg-turbo-3.2.0 -B jpeg-build -G Ninja -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=$CPU -DCMAKE_C_COMPILER="$CC" -DCMAKE_AR="$AR" -DCMAKE_RANLIB="$RANLIB" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$X" -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_SHARED=OFF \
        -DENABLE_STATIC=ON -DWITH_SIMD=0 -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF \
        -DWITH_TESTS=OFF >/dev/null
    ninja -C jpeg-build jpeg-static >/dev/null
    cp jpeg-build/libjpeg.a "$X/lib/"
    cp libjpeg-turbo-3.2.0/src/jpeglib.h libjpeg-turbo-3.2.0/src/jmorecfg.h \
       libjpeg-turbo-3.2.0/src/jerror.h jpeg-build/jconfig.h "$X/include/"
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: libjpeg\nDescription: libjpeg-turbo\nVersion: 3.2.0\nLibs: -L${libdir} -ljpeg\nCflags: -I${includedir}\n' \
        "$X" > "$X/lib/pkgconfig/libjpeg.pc"
    echo "=== expat"
    rm -rf expat-2.7.3 && tar xf "$SRC"/expat-2.7.3.tar.xz
    (cd expat-2.7.3 && ./configure --host=$TRIPLE CC=$CC --prefix="$X" --disable-shared \
       --enable-static --without-docs --without-examples --without-tests --without-xmlwf \
       >/dev/null && make -j"$J" >/dev/null && make install >/dev/null)
    echo "=== utf8proc"
    rm -rf utf8proc-2.12.0 && tar xf "$SRC"/utf8proc-2.12.0.tar.gz
    (cd utf8proc-2.12.0 && make CC="$CC" AR="$AR" libutf8proc.a >/dev/null &&
     cp libutf8proc.a "$X/lib/" && cp utf8proc.h "$X/include/" &&
     printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: libutf8proc\nDescription: utf8proc\nVersion: 2.12.0\nLibs: -L${libdir} -lutf8proc\nCflags: -I${includedir} -DUTF8PROC_STATIC\n' \
       "$X" > "$X/lib/pkgconfig/libutf8proc.pc")
}

libs() {
    rm -rf "$W/ws" && mkdir -p "$W/ws"
    for r in buildsystem libwapcaplet libparserutils libhubbub libdom libcss libnsgif libnsbmp \
             libnsutils libnspsl libnslog libsvgtiny nsgenbind netsurf; do
        mkdir -p "$W/ws/$r" && git -C "$NETSURF/$r" archive HEAD | tar -x -C "$W/ws/$r"
    done
    cd "$W/ws"
    local build; build=$(cc -dumpmachine)
    make -C buildsystem install PREFIX="$HOSTP" HOST="$build" >/dev/null
    make -C buildsystem install PREFIX="$X" HOST=$TRIPLE >/dev/null
    make -C nsgenbind -j"$J" install PREFIX="$HOSTP" HOST="$build" >/dev/null
    for lib in libwapcaplet libparserutils libhubbub libdom libcss libnsgif libnsbmp libnsutils \
               libnspsl libnslog libsvgtiny; do
        echo "=== $lib"
        make -C $lib -j"$J" install HOST=$TRIPLE PREFIX="$X" CC="$CC" AR="$AR" >/dev/null
    done
}

# content/fetch.c includes fetchers/curl.h whether or not curl is used, and
# that includes <curl/curl.h> for one type.  NetSurf here has no curl: this
# stands in for the header, and nothing else of curl is referred to.
curl_stub() {
    mkdir -p "$X/include/curl"
    printf '/* A stand-in for curl.h: NetSurf is built without curl (ports/netsurf/build.sh) */\ntypedef struct Curl_multi CURLM;\n' \
        > "$X/include/curl/curl.h"
}

# ROSGD's sources, beside NetSurf's: the native front end's directory
# (frontends/rosgd) and monkey's harness (frontends/rosgd-monkey)
ours() {
    rm -rf "$W/ws/netsurf/frontends/rosgd" "$W/ws/netsurf/frontends/rosgd-monkey"
    cp -R "$ROSGD/ports/netsurf/frontend" "$W/ws/netsurf/frontends/rosgd"
    mkdir -p "$W/ws/netsurf/frontends/rosgd-monkey"
    cp "$ROSGD/ports/netsurf/monkey/harness.c" "$W/ws/netsurf/frontends/rosgd-monkey/"
}

monkey() {
    echo "=== nsmonkey"
    curl_stub
    ours
    cd "$W/ws/netsurf"
    # monkey's own sources as its Makefile lists them, then ROSGD's fetcher
    # and harness: S_FRONTEND given whole, as the build would make it
    local srcs
    srcs=$(printf 'include frontends/monkey/Makefile\nrosgd-sources:\n\t@echo $(S_FRONTEND)\n' |
           make -s -f - FRONTEND_RESOURCES_DIR=frontends/monkey/res rosgd-sources)
    [ -n "$srcs" ] || { echo "build.sh: no sources from monkey's Makefile"; exit 1; }
    srcs="$(for s in $srcs; do printf 'frontends/monkey/%s ' "$s"; done)"
    srcs="$srcs frontends/rosgd/fetch_url.c frontends/rosgd-monkey/harness.c"
    # monkey prints a suseconds_t (long long on x32) with %ld under -Werror
    ROSGD_CFLAGS_EXTRA="-Wno-error=format" make -j"$J" TARGET=monkey HOST=$TRIPLE PREFIX="$X" CC="$CC" AR="$AR" \
        NETSURF_USE_ROSPRITE=NO NETSURF_USE_CURL=NO NETSURF_USE_OPENSSL=NO S_FRONTEND="$srcs"
    ls -l nsmonkey
}

# NetSurf with its Wimp front end, and !NetSurf: !Run, the program, !Sprites,
# and Resources (Messages split for it, NetSurf's style sheets and pages, its
# pointers)
rosgd() {
    echo "=== NetSurf, the Wimp front end"
    curl_stub
    ours
    cd "$W/ws/netsurf"
    make -j"$J" TARGET=rosgd VLDTARGET=rosgd HOST=$TRIPLE PREFIX="$X" CC="$CC" AR="$AR"
    local app="$W/!NetSurf" f
    rm -rf "$app"
    mkdir -p "$app/Resources"
    cp nsrosgd "$app/NetSurf,ff8"
    cp "$ROSGD/ports/netsurf/frontend/!Run" "$app/!Run,feb"
    cp "$ROSGD/ports/netsurf/frontend/!Boot" "$app/!Boot,feb"
    cp frontends/rosgd/res/en/Messages "$app/Resources/Messages"
    for f in default.css adblock.css quirks.css internal.css welcome.html credits.html licence.html \
             netsurf.png favicon.png; do
        cp -L "frontends/gtk/res/$f" "$app/Resources/$f"
    done
    cp -RL frontends/gtk/res/icons "$app/Resources/icons"
    # NetSurf's own RISC OS artwork: the application's icons, and its pointers
    cp 'frontends/riscos/appdir/!Sprites,ff9' 'frontends/riscos/appdir/!Sprites22,ff9' "$app/"
    cp frontends/riscos/appdir/Resources/Sprites,ff9 "$app/Resources/Sprites,ff9"
    # and the HTML file's icons, file_faf and small_faf, from RISC OS 5's own
    # desktop sprites (the Wimp's DiscSprites), which !Boot's IconSprites loads
    local disc="${RISCOSSRC:-$(beside riscos-src)/BCM2835/RiscOS}/Sources/Desktop/Wimp/Resources/UK/Ursula/DiscSprites"
    python3 "$ROSGD/ports/netsurf/tools/addsprites.py" "$app/!Sprites,ff9" "$disc/Sprites,ff9" file_faf small_faf
    python3 "$ROSGD/ports/netsurf/tools/addsprites.py" "$app/!Sprites22,ff9" "$disc/Sprites22,ff9" file_faf small_faf
    ls -l "$app"
}

# The checks' HTTP server: a static Linux program for *RunBox
httpd() {
    echo "=== httpd"
    zig cc -target $LINUX -static -O2 -Wall -o "$W/httpd" "$ROSGD/ports/netsurf/monkey/httpd.c"
    ls -l "$W/httpd"
}

case "${1:-all}" in
    deps) deps ;;
    libs) libs ;;
    monkey) monkey ;;
    httpd) httpd ;;
    rosgd) rosgd ;;
    all) deps; libs; monkey; httpd; rosgd ;;
    *) echo "build.sh [deps|libs|monkey|httpd|rosgd|all]"; exit 1 ;;
esac
