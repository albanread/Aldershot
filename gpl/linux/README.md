# Linux

Linux 6.18.54 from [kernel.org](https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.54.tar.xz), unchanged (`kernel/check-linux.sh` checks its SHA-256). GNU GPL 2, with the Linux syscall note.

BOX adds its configuration, and one driver of its own:

- `kernel/x86_64/config-x86_64.fragment`: the Intel Mac kernel.
- `kernel/x86_64/config-box.fragment`: added to the above for the Intel Mac kernel since 18 October 2026: the backings region.
- `kernel/x86_64/config-pc.fragment`: added to the above for the PC image.
- `kernel/aarch64/config-aarch64.fragment`: the Apple silicon kernel.
- `kernel/drivers/rosgd-shm.c`: the backings region, memory taken from the contiguous memory allocator that the Mac apps' QEMU draws the desktop's windows from (GNU GPL 2). `build-kernel.sh` copies it into the tree and builds it in.
- `kernel/build-kernel.sh`: configures and builds a kernel.
- `configs/`: the full `.config` of each released kernel.

To rebuild: unpack the kernel, copy in a `.config`, run `make olddefconfig`, then `make bzImage` (x86-64) or `make Image` (arm64).
