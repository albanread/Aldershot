#!/bin/sh
# build.sh -- the Paige engine and its RISC OS machine layer, as one
# archive a C application links (roscc link --clib), for either box.
#
#   ARCH=x86_64  (default) the Intel box: x32
#   ARCH=aarch64 the Apple Silicon box: A64X32
#
# Run from rosgd/ (sh ports/paige/build.sh).  The C compiler and its flags
# are capps.mk's table for the ABI, so the engine is built against the
# box's C library headers (build/<abi>/capps/inc/C), as the application
# that links it is.
#
#   1. PGHEADER/PGMTRAPS.H (third_party/Paige, committed there) carries a
#      ROSC_PLATFORM block: a memory_ref is a cell holding its block's
#      address, as a Mac Handle is (rosc/roscmem.c), over malloc.
#   2. PGPLATFO/PGROSC.H, force-included, is the platform shim: errno
#      names, the Mac fixed-point trio in plain C.
#   3. Every PGSOURCE + PGHLEVEL + PGDEBUG file, and PGPLATFO/PGMEMMGR.C,
#      compiles as C (-x c: the tree's .C suffix reads as C++ to clang).
#   4. rosc/pgrosc.c is the machine layer: fonts through the Font Manager,
#      drawing through OS_Plot and ColourTrans, files through OS_GBPB.
#
#   5. PGTXR, the RTF and HTML import and export, HERMES's C++: compiled as
#      C++ (no exceptions, no RTTI; rosc/cxxrt.c the runtime it needs).
#
# Output: build/ports/paige/libpaige.a (x32), build/aarch64/ports/paige/
# libpaige.a (A64X32); obj/ beside it.
set -eu

ROSGD=$(pwd)
HERE=$ROSGD/ports/paige
case "${ARCH:-x86_64}" in
x86_64)  B=build;         ABI=x32;    AR=$ROSGD/.cache/llvm/bin/llvm-ar ;;
aarch64) B=build/aarch64; ABI=a64x32; AR=$ROSGD/.cache/llvm/bin/llvm-ar ;;
*) echo "build.sh: ARCH is x86_64 or aarch64"; exit 1 ;;
esac
W=$ROSGD/$B/ports/paige
CAPPS=$ROSGD/build/$( [ $ABI = x32 ] && echo capps || echo a64x32/capps )
# The engine's source: HERMES-Paige at the commit the port starts from,
# with patches/engine-32bit-and-platform.patch on it (the box's commits in
# third_party/Paige, as one diff), made in .cache/paige and made again when
# the patch changes.  It is cloned from third_party/Paige when that is
# here, else from PAIGE_URL.  PAIGE=dir builds another tree as it stands
# (one being worked on).
PAIGE_URL=${PAIGE_URL:-https://github.com/nmatavka/HERMES-Paige.git}
PAIGE_BASE=cddd954
PATCH=$HERE/patches/engine-32bit-and-platform.patch
if [ -z "${PAIGE:-}" ]; then
    PAIGE=$ROSGD/.cache/paige
    sum=$(shasum -a 256 "$PATCH" 2>/dev/null || sha256sum "$PATCH")
    sum=${sum%% *}
    if [ "$(cat "$PAIGE/.patch-sha256" 2>/dev/null)" != "$sum" ]; then
        from=$ROSGD/../../third_party/Paige
        [ -d "$from/.git" ] || from=$PAIGE_URL
        echo "==> Paige: $PAIGE_BASE from $from, with the box's patch, into $PAIGE"
        rm -rf "$PAIGE" "$PAIGE.part"
        git clone -q "$from" "$PAIGE.part"
        git -C "$PAIGE.part" checkout -q "$PAIGE_BASE"
        git -C "$PAIGE.part" apply --whitespace=nowarn "$PATCH"
        echo "$sum" > "$PAIGE.part/.patch-sha256"
        mv "$PAIGE.part" "$PAIGE"
    fi
fi
grep -q ROSC_PLATFORM "$PAIGE/PGHEADER/PGMTRAPS.H" || {
    echo "build.sh: $PAIGE is Paige without the box's patch ($PATCH)"
    exit 1; }

[ -d "$PAIGE/PGSOURCE" ] || { echo "build.sh: no Paige sources ($PAIGE)"; exit 1; }
[ -f "$CAPPS/inc/C/kernel.h" ] || { echo "build.sh: no $CAPPS/inc/C (make ARCH=${ARCH:-x86_64} capps)"; exit 1; }
# an LLVM archiver (roscc reads its archives); zig's when the pinned one is absent
[ -x "$AR" ] || AR=$(command -v llvm-ar || echo "zig ar")

LINE=$(make -s -f capps.mk ABI=$ABI capps-cflags)
case "$LINE" in
"zig clang "*) CC="zig clang"; CFLAGS=${LINE#zig clang } ;;
*)             CC=$ROSGD/${LINE%% *}; CFLAGS=${LINE#* } ;;
esac
# the engine is 1990s C; its warnings are its own
CFLAGS="$CFLAGS -w -Wno-error -Wno-implicit-function-declaration -Wno-int-conversion \
    -Wno-incompatible-pointer-types -I$CAPPS/inc/C"
PGFLAGS="-DROSC_COMPILE -DROSC_PLATFORM -DPOSIX_PLATFORM -DUNICODE=1 \
    -I$HERE/PGPLATFO -I$PAIGE/PGHEADER -include PGROSC.H"

mkdir -p "$W/obj"
echo "==> Paige engine ($ABI)"
for f in "$PAIGE"/PGSOURCE/*.C "$PAIGE"/PGHLEVEL/*.C "$PAIGE"/PGDEBUG/*.C "$PAIGE"/PGPLATFO/PGMEMMGR.C; do
    $CC $CFLAGS $PGFLAGS -x c -c "$f" -o "$W/obj/$(basename "$f" .C).o"
done
echo "==> the RTF and HTML codecs (PGTXR)"
# HERMES's C++, compiled as C++ without exceptions or RTTI (rosc/cxxrt.c has
# new and delete); PG_TRY is setjmp under ROSC_PLATFORM, as in the engine.
# wchar_t is the C headers' typedef, as in C (-fno-wchar), and C++14 takes
# the files' 'register'.
CXXFLAGS=$(echo "$CFLAGS" | sed 's/-std=gnu11//; s/-fsanitize=integer-divide-by-zero//; s/-fsanitize-trap=integer-divide-by-zero//')
for f in PGIMPORT PGEXPORT PGRTFIMP PGRTFEXP PGNATIVE PGHTMIMP PGHTMEXP; do
    $CC $CXXFLAGS $PGFLAGS -x c++ -std=gnu++14 -Xclang -fno-wchar -fno-exceptions -fno-rtti \
        -fno-threadsafe-statics -c "$PAIGE/PGTXR/$f.CPP" -o "$W/obj/$f.o"
done
for f in PGRTFDEF PGDEFTBL PGHTMDEF PGPICT PGPICT2 PGWMF; do
    $CC $CFLAGS $PGFLAGS -x c -c "$PAIGE/PGTXR/$f.C" -o "$W/obj/$f.o"
done
echo "==> the machine layer"
for f in "$HERE"/rosc/*.c; do
    $CC $CFLAGS $PGFLAGS -c "$f" -o "$W/obj/$(basename "$f" .c).o"
done

rm -f "$W/libpaige.a"
$AR rc "$W/libpaige.a" "$W"/obj/*.o
echo "==> built $W/libpaige.a"
