export WLR_BACKENDS=drm WLR_RENDERER=pixman LIBSEAT_BACKEND=noop
/host/src/rosgd-compositor > /host/comp.log 2>&1 &
/usr/bin/busybox sleep 3
WAYLAND_DISPLAY=wayland-0 /usr/bin/foot -- /usr/bin/busybox sh > /host/foot.log 2>&1 &
