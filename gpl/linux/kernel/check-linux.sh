#!/bin/sh
# Check the kernel's source before it is unpacked (#36):
#
#   sh kernel/check-linux.sh VERSION [CACHE]
#
# CACHE/linux-VERSION.tar.xz (CACHE: rosgd/.cache) is put there by hand,
# so it is checked as deps/build-llvm.sh checks LLVM's: its SHA-256 must be
# the one pinned here.  kernel.org's kernel-sha256sums.asc, when it sits
# beside it, must list the same SHA-256, and with gpg its signature must be
# good by kernel.org's checksum autosigner -- when gpg has that key (gpg
# --locate-keys autosigner@kernel.org); without the key, or without gpg (the
# build VM), the signature is not checked and this says so.
#
# Run by kernel/build-kernel.sh in the build VM, and on the Mac by
# vm/buildvm.py and vm/build-kernel-aarch64.sh before they start it.
# A new kernel version needs its SHA-256 added below, from kernel.org's
# sha256sums.asc, its signature checked.
set -eu

V=${1:?usage: check-linux.sh VERSION [CACHE]}
here=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${2:-$here/.cache}
TAR=linux-$V.tar.xz
# kernel.org's checksum autosigner (autosigner@kernel.org), which signs
# sha256sums.asc
SIGNER=B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1

case $V in
6.18.54) PINNED=9df30b02dd8102bbd0be52556288ef6889ddbe7f1ddb96fbf847d0becf3eacac ;;
*) echo "check-linux: no SHA-256 pinned for $TAR (kernel/check-linux.sh)" >&2; exit 1 ;;
esac

sha256() { s=$(shasum -a 256 "$1" 2>/dev/null || sha256sum "$1"); echo "${s%% *}"; }

[ -f "$CACHE/$TAR" ] || { echo "check-linux: no $CACHE/$TAR (kernel.org's, put there by hand)" >&2; exit 1; }
got=$(sha256 "$CACHE/$TAR")
[ "$got" = "$PINNED" ] || {
    echo "check-linux: $TAR: SHA-256 $got, not the pinned $PINNED" >&2; exit 1; }

ASC=$CACHE/kernel-sha256sums.asc
if [ ! -f "$ASC" ]; then
    echo "== $TAR: SHA-256 good (no kernel-sha256sums.asc beside it: its list not checked)"
    exit 0
fi
listed=$(sed -n "s/^\([0-9a-f]\{64\}\)  $TAR\$/\1/p" "$ASC" | tr -d '\r')
[ "$listed" = "$PINNED" ] || {
    echo "check-linux: kernel-sha256sums.asc lists ${listed:-nothing} for $TAR, not the pinned $PINNED" >&2; exit 1; }
if ! command -v gpg > /dev/null 2>&1; then
    echo "== $TAR: SHA-256 good, as kernel-sha256sums.asc lists it (no gpg: its signature not checked)"
    exit 0
fi
status=$(gpg --batch --status-fd 1 --verify "$ASC" 2> /dev/null || true)
if echo "$status" | grep -q "^\[GNUPG:\] VALIDSIG $SIGNER "; then
    echo "== $TAR: SHA-256 good, as kernel-sha256sums.asc lists it, signature good"
elif echo "$status" | grep -q "^\[GNUPG:\] NO_PUBKEY "; then
    echo "== $TAR: SHA-256 good, as kernel-sha256sums.asc lists it (signature not checked:" \
         "gpg has not got the key $SIGNER; gpg --locate-keys autosigner@kernel.org)"
else
    echo "check-linux: kernel-sha256sums.asc: no good signature by $SIGNER" >&2
    exit 1
fi
