#!/bin/sh
# What a box's screen looks like, from another machine.
#
#   tools/boxshot.sh <host> [out.png]
#
# A PC managed over SSH (modules/sshd) has a screen nobody is sitting at.
# This asks the box to save it -- *ScreenSave, which writes it as a RISC OS
# sprite on its disc -- brings the file back over sftp, and makes a picture
# of it with tools/sprite2png.py.  out.png defaults to boxshot.png here.
#
# ROSGD_SSH holds extra options for both ssh and sftp, so they must be the
# ones both take: -i for a key and -o Port=<n> for a port (sftp's own port
# option is -P, not -p).  The file left on the box is its disc's
# ScreenShot, overwritten each time.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
host=${1:-}
out=${2:-boxshot.png}
[ -n "$host" ] || { echo "usage: $0 <host> [out.png]" >&2; exit 2; }
opts=${ROSGD_SSH:-}

# shellcheck disable=SC2086
ssh $opts "$host" 'ScreenSave HostFS::Disc.$.ScreenShot' >/dev/null
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# shellcheck disable=SC2086
sftp $opts "$host":/disc/ScreenShot,ff9 "$tmp/shot" >/dev/null
python3 "$here/tools/sprite2png.py" "$tmp/shot" "$out"
echo "$out: $(ls -l "$out" | awk '{print $5}') bytes"
