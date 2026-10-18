# kernel/aarch64

The Apple silicon box's kernel: Linux 6.18.54, `make ARCH=arm64 tinyconfig` plus `config-aarch64.fragment`.
Every driver is built in and there is no module loader.
It is the x86 box's kernel for Apple's Virtualization framework (VZ), which `vz/rosgd-vz` drives.
The fragment's groups are the x86 fragment's, with VZ's devices in place of QEMU's.
Each group says why it is there.

## Building it

```
vm/build-kernel-aarch64.sh [--cpus 10] [--memory 8192]
# -> .cache/out/Image-aarch64 (uncompressed: VZ boots no other kind)
#    .cache/out/config-aarch64
```

The script runs `kernel/build-kernel.sh box-aarch64` in an arm64 Alpine guest under `rosgd-vz`.
The guest is the one `vz/smoke.sh` makes, so run that once first.
The guest mounts `rosgd/` over virtio-fs, fetches gcc and the kernel's build tools from Alpine, builds, and powers off.
The tree is built on a tmpfs in the guest's RAM, because the Mac's volume is case-insensitive and the tree has names that differ only in case.
`build-kernel.sh` checks that the options the box needs survived `olddefconfig`, and that `MODULES` is off.

## What it gives the box

| For | In the guest |
| --- | --- |
| console | `hvc0` (virtio-console). VZ has no PL011. |
| pages | 4 KB pages on the Mac's 16 KB, with `vm.mmap_min_addr` 16384 |
| the share | `mount -t virtiofs host /host`. 9p is built in too, for QEMU's `virt`. |
| network | `eth0` (virtio-net), DHCP from the Mac's NAT |
| clock, entropy | `rtc0` (PL031) and `virtio_rng` |
| vsock | `/dev/vsock`, for the test harness |
| sound | ALSA card 0, "VirtIO SoundCard" |
| display | `/dev/dri/card0` (`virtio_gpu`), with fbcon on DRM's fbdev emulation and no `/dev/fb0` |
| keyboard, pointer | evdev, from Apple's virtual USB keyboard and digitizer |
| the polite stop | evdev "gpio-keys": `KEY_POWER` when the host asks VZ to stop |

`rosgd-vz`'s host-side stop calls `requestStop`, which presses a power button, a `gpio-keys` node in VZ's device tree.
Nothing in the kernel acts on it.
The box's input module shuts down when it sees it, and `rosgd-vz` forces the stop after its grace period if the guest does not power off.

`BINFMT_SCRIPT` is in, unlike on x86, because the guests that test the kernel run a shell script as `/init`.

## Tests

```
tests/a64x32/kernel.py [--gui] [--screenshot PNG]
tests/a64x32/arena.py --kernel .cache/out/Image-aarch64
```

`kernel.py` boots the Image and checks each row above over the console, then stops it with SIGTERM and checks that the key arrives and the guest powers off without a forced stop.
`arena.py` checks the arena on this kernel.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
