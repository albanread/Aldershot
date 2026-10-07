#!/bin/bash
# build.sh -- PipeDream 4 for the ROSGD box (x32; A64X32 with ARCH=aarch64).
#
# Compiles PipeDream's own units with the box's capps flag table against
# the staged stock 6.23 rlib headers, then links with roscc --clib
# against rlib.a and the ROM C library.
#
#   sh tools/build.sh compile   compile every unit (default)
#   sh tools/build.sh link      link build/!RunImage
#   sh tools/build.sh           both

set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
ROSGD=${ROSGD:-$(cd "$HERE/../.." && pwd)}
PDROOT=${PDROOT:-$HERE/pd/src}
ROSCC=${ROSCC:-$ROSGD/../roscc/target/release/roscc}

# ARCH=aarch64: the Apple Silicon box's A64X32 (design 26), with the box's
# pinned clang, against make ARCH=aarch64's staged headers, rlib.a and
# runtime (build/a64x32/capps); the image in build/aarch64/pipedream.
# Otherwise x32, as it always was.
case "${ARCH:-x86_64}" in
x86_64)
  OUT=${OUT:-$ROSGD/build/pipedream}
  CAPPS=$ROSGD/build/capps; RT=$ROSGD/build/gen/capps/rt; RT_ENV=ROSCC_X32_RT
  CC="zig clang"
  TARGET=(-target x86_64-linux-gnux32 -march=x86-64 -fno-pic)
  SAFETY=() ;;
aarch64)
  OUT=${OUT:-$ROSGD/build/aarch64/pipedream}
  CAPPS=$ROSGD/build/a64x32/capps; RT=$CAPPS/rt; RT_ENV=ROSCC_A64X32_RT
  CC="$ROSGD/.cache/llvm/bin/clang"
  # abi/a64x32/flags.mk's target, layout and safety additions
  TARGET=(-target aarch64-linux-gnu_ilp32 -fno-pic -ffixed-x18 -fwrapv-pointer -mno-outline-atomics
          -D__JMP_BUF_SIZE=44)
  SAFETY=(-fsanitize=integer-divide-by-zero -fsanitize-trap=integer-divide-by-zero) ;;
*) echo "build.sh: ARCH is x86_64 or aarch64"; exit 1 ;;
esac
# only the pinned, patched clang may build A64X32 (#38)
[ "${ARCH:-x86_64}" = x86_64 ] || sh "$ROSGD/abi/a64x32/check-clang.sh" "$CC" || exit 1

# The box's flag table (rosgd/capps.mk CAPPS_CFLAGS), PipeDream's
# cross-compile environment on top (Build/clang/clang-check.sh's defines,
# less the ARM target), and no SB_GLOBAL_REGISTER: include.h's fallback
# then keeps current_p_docu in global data.
FLAGS=("${TARGET[@]}"
       -funsigned-char -mlong-double-64 -fpack-struct=4 -mms-bitfields
       -ffp-contract=off -ffp-exception-behavior=maytrap -fno-builtin-memcpy -fno-strict-aliasing
       -fno-stack-protector -fno-sanitize=all "${SAFETY[@]}"
       -std=gnu11 -O2 -ffreestanding -nostdinc -fno-omit-frame-pointer
       -fno-color-diagnostics -fno-caret-diagnostics
       -D__riscos -D__APCS_32
       -DCROSS_COMPILE=1 -DHOST_CLANG=1 -DTARGET_RISCOS=1 -DRELEASED=1)

INC="-I$PDROOT -I$PDROOT/WimpLib \
     -I$CAPPS/inc/RISCOSLIB \
     -I$CAPPS/inc/tbox \
     -I$CAPPS/inc/C"

FORCEINC="-include $PDROOT/cmodules/coltsoft/target_riscos_host_box.h"

compile() {
  mkdir -p "$OUT/o"
  ok=0; fail=0; > "$OUT/failed.txt"
  for f in "$PDROOT"/*.c \
           "$PDROOT"/cmodules/*.c "$PDROOT"/cmodules/*/*.c \
           "$PDROOT"/WimpLib/*.c \
           "$PDROOT"/external/OtherFree/wn_pnpoly.c; do
    [ -f "$f" ] || continue
    case "$f" in
      "$PDROOT"/cmodules/coltsoft/*) ;;      # support headers only, no .c expected
      "$PDROOT"/WimpLib/nd-*) continue ;;    # ingest the rlib sources apply.sh copies in (not
                                             # shipped); the box's rlib.a provides those members
      "$PDROOT"/WimpLib/bbc.c|"$PDROOT"/WimpLib/event.c|"$PDROOT"/WimpLib/fileicon.c|\
      "$PDROOT"/WimpLib/resspr.c|"$PDROOT"/WimpLib/wimpt.c|"$PDROOT"/WimpLib/win.c|\
      "$PDROOT"/WimpLib/flex.c) continue ;; # ingested by the cs-*.c wrappers, not built alone
    esac
    b=$(echo "$f" | sed "s|$PDROOT/||; s|/|_|g; s|\.c$||")
    EXTRA=""
    case "$f" in
      "$PDROOT"/WimpLib/cs-*) EXTRA="-DCOMPILING_WIMPLIB_EXTENSIONS" ;;
    esac
    # shellcheck disable=SC2086
    if $CC "${FLAGS[@]}" $EXTRA $INC $FORCEINC -MMD -MF "$OUT/o/$b.d" -c "$f" -o "$OUT/o/$b.o" \
         > "$OUT/o/$b.log" 2>&1; then
      ok=$((ok+1)); rm -f "$OUT/o/$b.log"
    else
      fail=$((fail+1)); echo "$b" >> "$OUT/failed.txt"
    fi
  done
  echo "compiled: $ok OK, $fail FAIL (build/failed.txt; logs beside the objects)"
  [ "$fail" -eq 0 ]
}

link() {
  RL=$CAPPS/lib/rlib.a
  objs=$(ls "$OUT"/o/*.o | grep -v '\.log$')
  env $RT_ENV="$RT" "$ROSCC" link --clib -o "$OUT/!RunImage" $objs "$RL"
  echo "linked: $OUT/!RunImage"
}

case "${1:-compile}" in
  compile) compile ;;
  link) link ;;
  *) compile && link ;;
esac
