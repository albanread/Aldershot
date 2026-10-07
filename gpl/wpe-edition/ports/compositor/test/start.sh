export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
/host/src/rosgd-compositor > /host/comp.log 2>&1 &
/usr/bin/busybox sleep 2
WAYLAND_DISPLAY=wayland-0 /usr/lib/aarch64-linux-gnu/wpe-webkit-2.0/MiniBrowser file:///host/page.html > /host/client.log 2>&1 &
