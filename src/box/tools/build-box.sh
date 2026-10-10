#!/bin/sh
# Build the box whole: the ROM with its applications and resources in it,
# the applications that live on the disc, and the share a box in a window
# runs from.  This is the build to run; the steps below are what the
# README's make targets do one by one, in their order, and each makes only
# what has changed.
#
#   tools/build-box.sh            the box, ready for run/run-x86_64.sh
#   tools/build-box.sh --check    and its self-test, make boot, which must pass
#   tools/build-box.sh --image    and the PC disc and VMware machine (image/)
#
#   1. The tools the build runs that make does not build: rosasm
#      (../rosasm).  make builds roscc and rosbas itself.  RISC OS's C for
#      the applications (build/capps), which make stages only when it is
#      missing, is staged again when tools/capps.py or its patches changed.
#   2. make: build/initramfs.cpio, the ROM.  Its ResourceFS holds what
#      RISC OS 5.30's ROM holds, as its build lays it out
#      (tools/mkromapps.py, tools/mkresources.py):
#        Resources:$.Apps.!Chars, !Edit, !Draw
#            what the Apps icon opens: each one's !Boot, !Help and !Run
#        Resources:$.Resources.Chars, Edit, Draw
#            each one's program, !RunImage (Edit's and Draw's are x32
#            images, linked by roscc against the ROM C library; Chars's is
#            BASIC), and its Messages, Templates and Sprites, which its
#            !Run finds through <App>$Path
#        Resources:$.Resources.<module>
#            the modules' Messages and the desktop's own resources
#            (resources/, the Colours tables, the desktop sprites)
#      The desktop's own pieces are there too: the Display Manager on the
#      icon bar with its "display" sprite, the Task Manager's grey cog
#      ("taskmanager") in the Wimp's sprite pool, and the wallpaper,
#      Resources:$.Resources.Pinboard.Tile, which run/run-x86_64.sh sets
#      in a window (rosgd.backdrop).  Beside /init: the ROM C library's
#      image and the Linux programs the box runs (sshd, ksmbd, clang,
#      rosbas, roscc).  !Paint, !Help and !Alarm are not ROM applications
#      yet.  make builds !Write too (ports/write, over the Paige engine,
#      which ports/paige/build.sh clones and patches into third_party/Paige
#      when it is not there), for the share's Apps.
#   3. The applications that live on the disc, not in the ROM: !StrongED
#      (make stronged, from third_party's StrongED release) and !NetSurf
#      (make posix-libc, then ports/netsurf/build.sh, from third_party's
#      NetSurf clones).  Both are required: the build stops if their
#      sources are not there.
#   4. make usershare: build/usershare, the share run/run-x86_64.sh gives
#      a box in a window (HostFS::Host.$): disc/'s !Boot, Apps,
#      Examples (BASIC tokenised) and ReadMe, with !StrongED, !NetSurf
#      and !Write in its Apps.  What the user saved there stays.
#   5. --check: make boot.
#   6. --image: image/build-image.sh, whose RISC OS disc gets the same
#      Apps.
#   7. The desktop checked: each piece above, "ok" or "MISSING"; the build
#      fails if one is missing.
#
# A new worktree has no .cache and no QEMU.  The kernel (make kernel, in
# the Alpine build VM) and the programs deps/ builds (OpenSSL, curl,
# OpenSSH, ksmbd, tcc) are copied from ROSGD_CACHE_FROM, by default the
# main worktree's rosgd/.cache, rather than built again; QEMU is the main
# worktree's qemu/build-rosgd unless QEMU names one.  RISC OS's sources
# are BASICVPFSRC, by default riscos-src beside the toolchain.
set -eu
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
here=$(cd "$(dirname "$0")/.." && pwd)        # rosgd/, the script in its tools/
cd "$here"

check=0 image=0
for a in "$@"; do
    case $a in
        --check) check=1 ;;
        --image) image=1 ;;
        -h|--help) sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p;}' "$self"; exit 0 ;;
        *) echo "build-box.sh: unknown option $a (--check, --image)" >&2; exit 2 ;;
    esac
done

jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
main=$(git worktree list --porcelain | sed -n '1s/^worktree //p')/rosgd
fail() { echo "build-box.sh: $*" >&2; exit 1; }

# ---- what the build needs ---------------------------------------------

command -v zig >/dev/null || fail "no zig on PATH"
command -v python3 >/dev/null || fail "no python3 on PATH"
# The toolchain's own cargo, as the Makefile's CARGO: rustup's proxy on PATH
# has no rustlib beside it (#33)
cargo=${CARGO:-$(rustup which cargo 2>/dev/null || command -v cargo ||
                 ls "$HOME"/.rustup/toolchains/stable-*/bin/cargo 2>/dev/null | head -1)}
[ -x "$cargo" ] || fail "no cargo (rustup's stable toolchain)"
export CARGO=$cargo
[ -d "$(dirname "$cargo")/../lib/rustlib/x86_64-unknown-linux-musl" ] ||
    fail "no Rust musl target, which the box's rosbas and roscc need: rustup target add x86_64-unknown-linux-musl"

if [ -z "${BASICVPFSRC:-}" ]; then
    for d in "$here/../../riscos-src/BCM2835/RiscOS" "$main/../../riscos-src/BCM2835/RiscOS"; do
        [ -d "$d/Sources" ] && { BASICVPFSRC=$(cd "$d" && pwd); break; }
    done
fi
[ -d "${BASICVPFSRC:-}/Sources" ] || fail "no RISC OS sources: set BASICVPFSRC to riscos-src/BCM2835/RiscOS"
export BASICVPFSRC

# The kernel and deps/'s programs, from another tree's .cache if this one
# has none.  tcc is copied only if it was built from the same script and
# patch as this tree's, else make builds it again (from the tarballs,
# copied too).
from=${ROSGD_CACHE_FROM:-$main/.cache}
if [ "$(cd "$from" 2>/dev/null && pwd)" != "$here/.cache" ] && [ -d "$from" ]; then
    mkdir -p .cache
    for d in out openssl-musl curl-musl openssh-musl ksmbd-musl tcc; do
        [ -e ".cache/$d" ] || [ ! -d "$from/$d" ] && continue
        if [ "$d" = tcc ] && ! { cmp -s "$from/../deps/build-tcc.sh" deps/build-tcc.sh &&
                                 cmp -s "$from/../abi/x32/tcc-x32.patch" abi/x32/tcc-x32.patch; }; then
            cp -p "$from"/tinycc-*.tar.gz "$from"/musl-*.tar.gz .cache/ 2>/dev/null || true
            continue
        fi
        echo "== .cache/$d, from $from"
        cp -R "$from/$d" ".cache/$d"
    done
fi
[ -f .cache/out/bzImage-x86_64 ] ||
    fail "no kernel (.cache/out/bzImage-x86_64): make kernel builds it in the Alpine VM, or set ROSGD_CACHE_FROM"

if [ -z "${QEMU:-}" ]; then
    QEMU=$here/../qemu/build-rosgd/qemu-system-x86_64
    [ -x "$QEMU" ] || QEMU=$main/../qemu/build-rosgd/qemu-system-x86_64
fi
export QEMU

# ---- 1. rosasm -----------------------------------------------------------

echo "== rosasm"
(cd ../rosasm && PATH="$(dirname "$cargo"):$PATH" "$cargo" build --release -q)

# ---- 2. the ROM ----------------------------------------------------------

# RISC OS's C for the applications (build/capps) is staged once, when it is
# not there, so a tree staged before tools/capps.py or its patches changed
# keeps the old headers (math.h before e99b08f2a0: Draw's image then has
# __d_floor undefined).  Staged again when those two files are not the
# ones it was staged with.
stage_id=$(cat tools/capps.py tools/capps_patches.py | shasum | cut -c1-40)
if [ -d build/capps ] && [ "$(cat build/capps/.staged-by 2>/dev/null)" != "$stage_id" ]; then
    echo "== build/capps was staged by another tools/capps.py: staged again"
    rm -rf build/capps
fi

echo "== the ROM: make"
make -j"$jobs"
echo "$stage_id" > build/capps/.staged-by

# ---- 3. the disc's applications -----------------------------------------

# third_party (StrongED's release, NetSurf's clones) beside the toolchain,
# as riscos-src is: this tree's, else the main worktree's (#34)
if [ -z "${THIRD_PARTY:-}" ]; then
    THIRD_PARTY=$here/../../third_party
    for d in "$here/../../third_party" "$main/../../third_party"; do
        [ -d "$d" ] && { THIRD_PARTY=$(cd "$d" && pwd); break; }
    done
fi
export THIRD_PARTY
stronged_app=${STRONGED_APP:-$THIRD_PARTY/StrongED/app/StrongED/!StrongED}
[ -d "$stronged_app" ] || fail "no StrongED release at $stronged_app (STRONGED_APP), which !StrongED is made from"
echo "== !StrongED: make stronged"
make -j"$jobs" stronged STRONGED_APP="$stronged_app"

netsurf=${NETSURF:-$THIRD_PARTY/NetSurf}
[ -d "$netsurf/netsurf" ] || fail "no NetSurf clones at $netsurf (NETSURF), which !NetSurf is built from"
echo "== !NetSurf: make posix-libc, ports/netsurf/build.sh"
make -j"$jobs" posix-libc
# The libraries once; after that the front end and !NetSurf
if [ -d build/ports/netsurf/ws/netsurf ] && [ -d "build/ports/netsurf/!NetSurf" ]; then
    NETSURF=$netsurf sh ports/netsurf/build.sh rosgd
else
    NETSURF=$netsurf sh ports/netsurf/build.sh all
fi

# ---- 4. the user's share -------------------------------------------------

echo "== the share: make usershare"
make usershare

# ---- 5, 6. the self-test, the PC image -----------------------------------

if [ $check = 1 ]; then
    echo "== the self-test: make boot"
    make boot
fi
if [ $image = 1 ]; then
    echo "== the PC image: image/build-image.sh"
    sh image/build-image.sh "$here/build/initramfs.cpio"
fi

# ---- what the box must have ----------------------------------------------

# The desktop as the user has it: the build fails if any is missing
missing=
need() { if eval "$2"; then echo "  ok       $1"; else echo "  MISSING  $1"; missing="$missing
  $1"; fi; }
sprites() {     # the sprite names in a sprite file
    python3 -c 'import struct,sys
d=open(sys.argv[1],"rb").read(); n,o=struct.unpack_from("<2I",d); o-=4
for _ in range(n): print(d[o+4:o+16].split(b"\0")[0].decode("latin1")); o+=struct.unpack_from("<I",d,o)[0]' "$1"
}
echo
echo "The desktop:"
need "the Display Manager, on the icon bar (modules/display)" \
    "LC_ALL=C grep -q 'DisplayManager:Messages' build/init"
need "its icon, the Wimp's \"display\" sprite" \
    "sprites resources/Resources/Wimp/Sprites,ff9 | grep -qx display"
need "the Task Manager's grey cog, the Wimp's \"taskmanager\" sprite" \
    "sprites resources/Resources/Wimp/Sprites,ff9 | grep -qx taskmanager"
need "the wallpaper, Resources:\$.Resources.Pinboard.Tile, set in a window (rosgd.backdrop)" \
    "[ -f resources/Resources/Pinboard/Tile,ff9 ] && grep -q 'rosgd.backdrop' run/run-x86_64.sh"
for a in Chars Edit Draw; do
    need "Resources:\$.Apps.!$a, in the ROM" "[ -d build/gen/romapps/Apps/!$a ] && [ -d build/gen/romapps/Resources/$a ]"
done
for a in StrongED NetSurf Write; do
    need "Apps.!$a, on the share" "[ -f 'build/usershare/Apps/!$a/!Run,feb' ]"
done
[ -z "$missing" ] || fail "the box is missing:$missing"

# ---- what was built ------------------------------------------------------

echo
echo "The ROM, build/initramfs.cpio:"
for a in build/gen/romapps/Apps/*; do
    n=${a##*/}
    echo "  Resources:\$.Apps.$n    Resources:\$.Resources.${n#!}: $(ls "build/gen/romapps/Resources/${n#!}" | tr '\n' ' ')"
done
echo "The share, build/usershare (HostFS::Host.\$):"
echo "  Apps: $(ls build/usershare/Apps | tr '\n' ' ')"
echo "  $(ls build/usershare | tr '\n' ' ')"
echo "Run it: run/run-x86_64.sh"
