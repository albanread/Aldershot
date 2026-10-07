export WLR_BACKENDS=drm WLR_RENDERER=pixman LIBSEAT_BACKEND=noop
/usr/bin/busybox ls -la /dev/dri > /host/comp.log 2>&1
/host/src/rosgd-compositor >> /host/comp.log 2>&1 &
/usr/bin/busybox sleep 3
WAYLAND_DISPLAY=wayland-0 /usr/lib/aarch64-linux-gnu/wpe-webkit-2.0/MiniBrowser file:///host/page.html > /host/client.log 2>&1 &
