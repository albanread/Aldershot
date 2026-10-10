#!/bin/bash
# desksnap.sh <out.png> [secs] -- a picture of the desktop, hosted.
#
# Runs *Desktop in build/hosttest with a boot file whose last task, Snap,
# polls for [secs] (default 3), saves the screen (OS_SpriteOp 2) and writes
# a flag; then the screen becomes a PNG (tools/sprite2png.py).  The share is
# a fresh directory under $TMPDIR, HostFS's disc "HostFS".
#
#   CLICK=<buttons>  after the wait, that click (4 Select, 2 Menu, 1 Adjust)
#   ICON=<n>         on icon-bar icon handle n (default 0: the first made,
#                    the Task Manager's), sent as Wimp_SendMessage 6 to
#                    window -2; then a second's polling before the save
#   STAGE=<dir>      its contents copied into the share first
#   PRE=<command>    a line of the boot file before Snap (each line is
#                    started as a task)
#   HOSTTEST=<file>  another build's binary (default: this tree's)
#   KEEP=1           keep the run directory (the share, the output, the
#                    SWI counts) even when the picture was made
#
# e.g. tools/desksnap.sh /tmp/desk.png
#      CLICK=4 ICON=0 tools/desksnap.sh /tmp/tasks.png   (the Task Manager's window)
#
# Its stdin is a FIFO of its own, and it kills only what it started.
set -u
if [ $# -lt 1 ]; then
    sed -n '2,3p' "$0" | sed 's/^# //' >&2
    exit 2
fi
HERE=$(cd "$(dirname "$0")" && pwd)
ROSGD=$(dirname "$HERE")
BIN=${HOSTTEST:-$ROSGD/build/hosttest}
OUT=$1
SECS=${2:-3}
CLICK=${CLICK:-0}
ICON=${ICON:-0}
[ -x "$BIN" ] || { echo "desksnap: no $BIN (make build/hosttest first)" >&2; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/desksnap.XXXX")
mkdir -p "$W/share"
cat > "$W/share/snap,fff" <<BAS
10 SYS "Wimp_Initialise",310,&4B534154,"Snap",0 TO ,t%
20 DIM b% 255: T%=TIME+$((SECS*100))
30 REPEAT: SYS "Wimp_Poll",0,b% TO r%: UNTIL TIME>T%
40 IF $CLICK=0 THEN 90
50 b%!0=-2: b%!4=$ICON: SYS "Wimp_GetIconState",,b%
60 x%=(b%!8+b%!16) DIV 2: y%=(b%!12+b%!20) DIV 2
70 !b%=x%: b%!4=y%: b%!8=$CLICK: b%!12=-2: b%!16=$ICON: SYS "Wimp_SendMessage",6,b%,-2,$ICON
80 T%=TIME+100: REPEAT: SYS "Wimp_Poll",0,b%+32 TO r%: UNTIL TIME>T%
90 SYS "OS_SpriteOp",2,0,"HostFS:\$.desk",0
100 SYS "OS_File",10,"HostFS:\$.done",&FFF,,0,0
110 REPEAT: SYS "Wimp_Poll",0,b% TO r%: UNTIL FALSE
BAS
[ -n "${STAGE:-}" ] && cp -R "$STAGE"/. "$W/share/"
{ [ -n "${PRE:-}" ] && printf '%s\n' "$PRE"; printf 'BASIC -quit HostFS:$.snap\n'; } > "$W/share/DFile,fff"
printf 'WimpSlot 4M\nWimpTask Desktop -File HostFS:$.DFile\n' > "$W/share/Run,feb"
F=$W/in.fifo
mkfifo "$F"
(sleep 120 > "$F") & SP=$!
( cd "$ROSGD" && ROSGD_SWISTATS="$W/stats.txt" ROSGD_DISCS="HostFS=$W/share" \
    ROSGD_CLI='Obey HostFS:$.Run' exec "$BIN" < "$F" > "$W/out.txt" 2>&1 ) & P=$!
for i in $(seq 1 $((SECS*10+300))); do
    [ -f "$W/share/done,fff" ] && break
    kill -0 $P 2>/dev/null || break
    sleep 0.1
done
sleep 0.3
kill $P 2>/dev/null
kill $SP 2>/dev/null
wait $P 2>/dev/null
wait $SP 2>/dev/null
if [ -f "$W/share/desk,ff9" ]; then
    python3 "$HERE/sprite2png.py" "$W/share/desk,ff9" "$OUT" || { echo "desksnap: run dir $W" >&2; exit 1; }
    if [ "${KEEP:-0}" = 1 ]; then echo "desksnap: run dir $W"; else rm -rf "$W"; fi
else
    echo "desksnap: no screen saved; run dir $W" >&2
    tail -5 "$W/out.txt" >&2
    exit 1
fi
