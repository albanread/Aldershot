#!/bin/sh
# build.sh -- !Write, the word processor, for either box: the Toolbox
# front end (src/), the Paige engine and its machine layer
# (ports/paige/build.sh's libpaige.a), and SQLite for the spelling
# (ports/rossqlite's amalgamation and RISC OS VFS), linked as a C
# application against the box's C library (roscc link --clib).
#
#   ARCH=x86_64  (default) the Intel box: x32
#   ARCH=aarch64 the Apple Silicon box: A64X32
#
# Run from rosgd/ (make write, make ARCH=aarch64 write).  Output:
# build/ports/write/!Write (x32), build/aarch64/ports/write/!Write
# (A64X32), which make usershare puts in Apps.
set -eu

ROSGD=$(pwd)
HERE=$ROSGD/ports/write
case "${ARCH:-x86_64}" in
x86_64)  B=build;         ABI=x32 ;;
aarch64) B=build/aarch64; ABI=a64x32 ;;
*) echo "build.sh: ARCH is x86_64 or aarch64"; exit 1 ;;
esac
if [ $ABI = x32 ]; then
    export ROSCC_X32_RT=$ROSGD/build/gen/capps/rt
else
    export ROSCC_A64X32_RT=$ROSGD/build/a64x32/capps/rt
fi
W=$ROSGD/$B/ports/write
CAPPS=$ROSGD/build/$( [ $ABI = x32 ] && echo capps || echo a64x32/capps )
ROSCC=$ROSGD/../roscc/target/release/roscc
# the engine's source: ports/paige/build.sh's (.cache/paige), or PAIGE's
PAIGE_SET=${PAIGE:-}
PAIGE=${PAIGE:-$ROSGD/.cache/paige}
LIBPAIGE=$ROSGD/$B/ports/paige/libpaige.a
APP=$W/!Write

[ -f "$CAPPS/inc/C/kernel.h" ] || { echo "build.sh: no $CAPPS/inc/C (make ARCH=${ARCH:-x86_64} capps)"; exit 1; }
[ -x "$ROSCC" ] || { echo "build.sh: no roscc ($ROSCC)"; exit 1; }

# the engine first (its own build.sh)
ARCH=${ARCH:-x86_64} PAIGE=$PAIGE_SET sh "$ROSGD/ports/paige/build.sh"

LINE=$(make -s -f capps.mk ABI=$ABI capps-cflags)
case "$LINE" in
"zig clang "*) CC="zig clang"; CFLAGS=${LINE#zig clang } ;;
*)             CC=$ROSGD/${LINE%% *}; CFLAGS=${LINE#* } ;;
esac
CFLAGS="$CFLAGS -I$CAPPS/inc/C -Wno-shift-op-parentheses -Wno-shift-negative-value"
PGFLAGS="-DROSC_COMPILE -DROSC_PLATFORM -DPOSIX_PLATFORM -DUNICODE=1 \
    -I$ROSGD/ports/paige/PGPLATFO -I$PAIGE/PGHEADER -include PGROSC.H -w"

# the spelling's SQLite: ports/rossqlite's amalgamation and RISC OS VFS
SQL=$ROSGD/ports/rossqlite
SQLFLAGS="-w -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_MAX_MMAP_SIZE=0 \
    -DSQLITE_OMIT_WAL -I$SQL/vendor"

mkdir -p "$W/obj"
echo "==> !Write ($ABI)"
if [ ! -f "$W/obj/sqlite3.o" ] || [ "$SQL/vendor/sqlite3.c" -nt "$W/obj/sqlite3.o" ]; then
    echo "    sqlite3.c (once)"
    $CC $CFLAGS $SQLFLAGS -c "$SQL/vendor/sqlite3.c" -o "$W/obj/sqlite3.o"
fi
$CC $CFLAGS $SQLFLAGS -c "$SQL/module/rosvfs.c" -o "$W/obj/rosvfs.o"
$CC $CFLAGS -I$SQL/vendor -c "$HERE/src/spell.c" -o "$W/obj/spell.o"
$CC $CFLAGS -c "$HERE/src/main.c" -o "$W/obj/main.o"
$CC $CFLAGS $PGFLAGS -c "$HERE/src/doc.c" -o "$W/obj/doc.o"

rm -rf "$APP"
mkdir -p "$APP"
"$ROSCC" link --clib -o "$APP/!RunImage,ff8" "$W/obj/main.o" "$W/obj/doc.o" \
    "$W/obj/spell.o" "$W/obj/rosvfs.o" "$W/obj/sqlite3.o" "$LIBPAIGE"
cp "$HERE/res/!Write/!Run" "$APP/!Run,feb"
cp "$HERE/res/!Write/!Boot" "$APP/!Boot,feb"
cp "$HERE/res/!Write/!Help" "$APP/!Help,fff"
cp "$HERE/res/!Write/Messages" "$APP/Messages,fff"
python3 "$HERE/tools/mkwrite.py" "$APP/Res,fae"
python3 "$HERE/tools/mksprites.py" "$APP/!Sprites,ff9" "$APP/Sprites,ff9"
# the document layer's test, without the desktop (test/engine.py runs it)
$CC $CFLAGS -c "$HERE/test/engine.c" -o "$W/obj/engine.o"
"$ROSCC" link --clib -o "$W/EngineTest,ff8" "$W/obj/engine.o" "$W/obj/doc.o" \
    "$W/obj/spell.o" "$W/obj/rosvfs.o" "$W/obj/sqlite3.o" "$LIBPAIGE"
if [ $ABI = a64x32 ]; then
    "$ROSCC" a64x32-lint "$APP/!RunImage,ff8" "$W/EngineTest,ff8"
fi
echo "==> built $APP"
