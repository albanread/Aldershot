# The Apple Silicon box's kernel

Linux 6.18.54, `make ARCH=arm64 tinyconfig` plus `config-aarch64.fragment`,
every driver built in and no module loader. It is the x86 box's kernel
for Apple's Virtualization framework (VZ), which `vz/rosgd-vz` drives
(RISCOSGrandDesign design 26 §4, package P2). The fragment's groups are the
x86 fragment's, with VZ's devices in place of QEMU's; each says why it is
there.

## Building it

    vm/build-kernel-aarch64.sh [--cpus 10] [--memory 8192]
    # -> .cache/out/Image-aarch64 (uncompressed: VZ boots no other kind)
    #    .cache/out/config-aarch64

It runs `kernel/build-kernel.sh box-aarch64` natively in an arm64 Alpine
guest under `rosgd-vz`. The guest is `vz/smoke.sh`'s (Alpine 3.24.2's stock
kernel and minirootfs; run `vz/smoke.sh` once), with `vm/aarch64/init`
appended to its initramfs as a second cpio archive. That `/init` mounts
`rosgd/` over virtio-fs at `/mnt/rosgd`, fetches gcc and the kernel's
build tools from Alpine over VZ's NAT (kept in `.cache/apk-aarch64`),
builds, and powers off. The tarball is the x86 build's,
`.cache/linux-6.18.54.tar.xz`.

**The tree is built on a tmpfs** in the guest's RAM (6 GB of the 8). The
Mac's volume is case-insensitive and the tree has names that differ only
in case, so it cannot be built on the share. A tmpfs needs no disc image
and no change to `rosgd-vz` (which has no block device option), and leaves
nothing behind; the cost is a full build each time, which is short.

On the Studio (M4 Max), 10 vCPUs and 8 GB: 70 s for the build, 76–83 s
from launch to power-off with the packages cached. The `Image` is 7.6 MB
(7,577,608 bytes).

`build-kernel.sh` checks by name that the options the box needs survived
`olddefconfig`, and that `MODULES` is off.

## What it gives the box

| For | In the guest |
|---|---|
| the console | `hvc0` (virtio-console); `console=hvc0`. VZ has no PL011, so no `earlycon` or `ttyAMA0` |
| pages | 4 KB (`ARM64_4K_PAGES`) on the Mac's 16 KB; `vm.mmap_min_addr` 16384 from boot |
| the share | `mount -t virtiofs host /host` (tag `host`); 9p is built in too, for QEMU's `virt` |
| network | `eth0`, virtio-net; DHCP from the Mac's NAT (192.168.64.x), by `ip=dhcp` or a client |
| the clock | `rtc0`, the PL031, read into the system clock at boot |
| entropy | `virtio_rng` |
| vsock | `/dev/vsock`, for the harness (P3h) |
| sound | ALSA card 0, "VirtIO SoundCard" (`/dev/snd/pcmC0D0p`) |
| display (`--gui`) | `/dev/dri/card0`, `virtio_gpu`; fbcon on DRM's fbdev emulation; no `/dev/fb0` (`FB_DEVICE` off, as on x86) |
| keyboard, pointer (`--gui`) | evdev: "Apple Inc. Virtual USB Keyboard" (keys) and "Apple Inc. Virtual USB Digitizer" (keys, `EV_REL` and `EV_ABS`), by xHCI and usbhid |
| the polite stop | evdev "gpio-keys": `KEY_POWER` (116) when the host asks VZ to stop |

**VZ's stop is a key.** `rosgd-vz`'s host-side stop (SIGTERM, SIGINT, the
window, `--seconds`) calls `requestStop`, which presses a power button:
a `gpio-keys` node on the PL061 in VZ's device tree. It arrives as
`EV_KEY KEY_POWER 1` on the device named `gpio-keys`, from boot
`/dev/input/event0`. Nothing in the kernel acts on it; the box's input
module shuts down when it sees it, and `rosgd-vz` forces the stop after its
grace period if the guest does not power off. The USB keyboard also
declares `KEY_POWER`, so the input module should tell the two by name.

## Checked

    tests/a64x32/kernel.py [--gui] [--screenshot PNG]
    tests/a64x32/arena.py --kernel .cache/out/Image-aarch64

`kernel.py` boots the Image with smoke's initramfs and checks each row
above over the console, then sends `rosgd-vz` SIGTERM with
`tests/a64x32/kernel/evkey.c` waiting in the guest for `KEY_POWER`: the
key arrives and the guest powers off 0.2 s later, with no forced stop.
On 30 September 2026: `/init` ran 0.08 s into the kernel's boot and 0.26 s
after `rosgd-vz` started (0.4 s with the window). `arena.py` (design 26
P1) passes on this kernel, all three runs.

`BINFMT_SCRIPT` is in, unlike on x86: the guests that test the kernel run
a shell script as `/init`.
