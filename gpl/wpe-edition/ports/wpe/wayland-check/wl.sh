export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
/usr/bin/cage -- /usr/lib/aarch64-linux-gnu/wpe-webkit-2.0/MiniBrowser file:///host/page.html > /host/cage.log 2>&1 &
pid=$!
/usr/bin/busybox sleep 12
ls /run/user >> /host/cage.log 2>&1
WAYLAND_DISPLAY=wayland-0 /usr/bin/grim /host/wayland-shot.png >> /host/cage.log 2>&1
echo "grim exit $?" >> /host/cage.log
kill $pid
