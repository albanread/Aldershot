# Linux

The kernel every BOX runs on: Linux **6.18.54**, from
[kernel.org](https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.54.tar.xz),
**unchanged** (`kernel/check-linux.sh` checks its SHA-256). GNU GPL 2, with the
Linux syscall note.

What the BOX adds is its configuration, here:

* `kernel/x86_64/config-x86_64.fragment` -- the Intel box (the Intel Mac app);
  `kernel/x86_64/config-pc.fragment` on top of it -- the PC image;
  `kernel/aarch64/config-aarch64.fragment` -- the Apple silicon box (the Apple
  silicon Mac app). Each is merged over `make tinyconfig`.
* `kernel/build-kernel.sh` -- how each kernel is configured and built (in a Linux
  build VM). The PC kernel has the BOX's ROM built in as its initramfs.
* `configs/` -- the full `.config` each released kernel was built with:
  `config-aarch64-vz-qemu`, `config-x86_64-qemu`, and `config-x86_64-pc` (the PC
  image of 7 October 2026, whose command line adds `rosgd.display=compositor`).

To rebuild one: unpack linux-6.18.54, copy the `.config` in, `make olddefconfig`,
and `make bzImage` (x86-64) or `make Image` (arm64). The PC kernel's initramfs is
the BOX's ROM, `CONFIG_INITRAMFS_SOURCE`.
