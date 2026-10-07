#!/bin/sh
# Build the ROSGD guest kernel.  Runs inside the build VM (vm/buildvm.py),
# as root, with this rosgd/ directory shared at /mnt/rosgd over 9p and a
# case-sensitive ext4 disc at /mnt/build -- the host's HFS+ volume is
# case-insensitive, and the kernel tree has files that differ only in case.
#
#   sh /mnt/rosgd/kernel/build-kernel.sh           the QEMU box's kernel
#   sh /mnt/rosgd/kernel/build-kernel.sh pc        the PC's (image/): the box's
#                                                  plus x86_64/config-pc.fragment, with
#                                                  .cache/pc/initramfs.cpio, the
#                                                  ROM, built in
#   sh /mnt/rosgd/kernel/build-kernel.sh box-aarch64
#                                                  the Apple Silicon box's
#                                                  (arm64, for VZ; in an arm64
#                                                  guest, vm/build-kernel-aarch64.sh):
#                                                  aarch64/config-aarch64.fragment
#
# Output: /mnt/rosgd/.cache/out/{bzImage-x86_64,config-x86_64}, or for pc,
# {bzImage-pc-x86_64,config-pc-x86_64}, or for box-aarch64
# {Image-aarch64,config-aarch64} -- an uncompressed Image, the only kind
# VZLinuxBootLoader boots.  The pc kernel builds in a tree of
# its own, so neither build undoes the other's objects (and the kernel will
# not build out of a tree that has built in place).
set -eu

V=6.18.54
FLAVOUR=${1:-box}
ROSGD=/mnt/rosgd
OUT=$ROSGD/.cache/out
FRAGMENT=$ROSGD/kernel/x86_64/config-x86_64.fragment
mkdir -p "$OUT"
# x86-64 unless the flavour says otherwise
ARCH=x86_64
TARGET=bzImage
IMAGE=arch/x86/boot/bzImage

case "$FLAVOUR" in
box)
    BASE=/mnt/build
    SUFFIX=x86_64
    FRAGMENTS=$FRAGMENT ;;
pc)
    BASE=/mnt/build/pc
    SUFFIX=pc-x86_64
    FRAGMENTS="$FRAGMENT $ROSGD/kernel/x86_64/config-pc.fragment"
    # The ROM, copied in from the 9p share: the build reads it more than
    # once, and it must not change underneath
    mkdir -p "$BASE"
    cp "$ROSGD/.cache/pc/initramfs.cpio" "$BASE/rosgd-initramfs.cpio"
    # its path, as one more fragment (scripts/config needs bash, which the
    # build VM has not got)
    echo "CONFIG_INITRAMFS_SOURCE=\"$BASE/rosgd-initramfs.cpio\"" > "$BASE/initramfs.fragment"
    FRAGMENTS="$FRAGMENTS $BASE/initramfs.fragment"
    # and an image's own changes, last (image/build-image.sh writes it: the
    # WPE edition's command line)
    [ -f "$ROSGD/.cache/pc/extra.fragment" ] && FRAGMENTS="$FRAGMENTS $ROSGD/.cache/pc/extra.fragment"
    : ;;
box-aarch64)
    BASE=/mnt/build/aarch64
    SUFFIX=aarch64
    FRAGMENTS=$ROSGD/kernel/aarch64/config-aarch64.fragment
    ARCH=arm64
    TARGET=Image
    IMAGE=arch/arm64/boot/Image
    mkdir -p "$BASE" ;;
*)
    echo "ROSGD-KERNEL-FAILED: no flavour $FLAVOUR"
    exit 1 ;;
esac
TREE=$BASE/linux-$V
OBJ=$TREE
if [ ! -d "$TREE" ]; then
    # its SHA-256 pinned, kernel.org's signed list when it is there (#36)
    sh "$ROSGD/kernel/check-linux.sh" "$V" ||
        { echo "ROSGD-KERNEL-FAILED: linux-$V.tar.xz is not the one pinned"; exit 1; }
    echo "== unpacking linux-$V into $BASE"
    tar -xJf "$ROSGD/.cache/linux-$V.tar.xz" -C "$BASE"
fi
cd "$TREE"
K="make ARCH=$ARCH"

echo "== configuring $FLAVOUR: tinyconfig + $FRAGMENTS"
# Every option a fragment names must be one this Linux has: kconfig drops
# an unknown name without a word (CONFIG_SND_VIRTIOSND, #35)
find . -name 'Kconfig*' -type f | xargs sed -n 's/^[[:space:]]*\(menu\)\{0,1\}config[[:space:]]\{1,\}\([A-Za-z0-9_]*\).*/\2/p' |
    sort -u > "$BASE/kconfig-symbols"
unknown=$(sed -n 's/^CONFIG_\([A-Za-z0-9_]*\)=.*/\1/p; s/^# CONFIG_\([A-Za-z0-9_]*\) is not set.*/\1/p' $FRAGMENTS |
    sort -u | grep -vxF -f "$BASE/kconfig-symbols" || true)
if [ -n "$unknown" ]; then
    for opt in $unknown; do echo "UNKNOWN CONFIG_$opt (not an option of Linux $V)"; done
    echo "ROSGD-KERNEL-FAILED: the fragments name options Linux has not got"
    exit 1
fi
$K tinyconfig > /dev/null
scripts/kconfig/merge_config.sh -m .config $FRAGMENTS > /dev/null
$K olddefconfig > /dev/null

# The options the runtime cannot do without.  A dependency the fragment
# forgot makes olddefconfig drop an option silently, and the guest then
# boots without, say, its display -- so each is checked by name.
if [ "$ARCH" = arm64 ]; then
REQUIRED="BINFMT_ELF BINFMT_SCRIPT BLK_DEV_INITRD DEVTMPFS PROC_FS PROC_SYSCTL SYSFS SHMEM TMPFS
          MEMFD_CREATE FUTEX EPOLL TIMERFD INOTIFY_USER ARM64_4K_PAGES ARM_PSCI_FW
          RTC_DRV_PL031 GPIO_PL061 KEYBOARD_GPIO VIRTIO_CONSOLE
          PCI PCI_HOST_GENERIC VIRTIO_PCI DRM DRM_VIRTIO_GPU DRM_FBDEV_EMULATION
          FRAMEBUFFER_CONSOLE INPUT_EVDEV USB_XHCI_HCD USB_XHCI_PCI USB_HID HID_GENERIC
          NET INET IPV6 UNIX PACKET VIRTIO_NET IP_PNP IP_PNP_DHCP VSOCKETS VIRTIO_VSOCKETS
          FUSE_FS VIRTIO_FS NET_9P_VIRTIO 9P_FS VIRTIO_BLK HW_RANDOM_VIRTIO
          SND SND_PCM SND_VIRTIO
          UNIX98_PTYS POSIX_TIMERS SECCOMP SECCOMP_FILTER CIFS SMB_SERVER NLS_UTF8
          NAMESPACES USER_NS PID_NS NET_NS UTS_NS SYSVIPC IPC_NS NO_HZ_IDLE HIGH_RES_TIMERS
          ADVISE_SYSCALLS MEMBARRIER RSEQ"
else
REQUIRED="BINFMT_ELF BLK_DEV_INITRD DEVTMPFS PROC_FS SYSFS SHMEM TMPFS
          MEMFD_CREATE FUTEX EPOLL TIMERFD INOTIFY_USER SERIAL_8250 SERIAL_8250_CONSOLE
          ACPI PCI VIRTIO_PCI DRM DRM_VIRTIO_GPU INPUT_EVDEV VIRTIO_INPUT
          NET INET IPV6 VIRTIO_NET IP_PNP IP_PNP_DHCP NET_9P NET_9P_VIRTIO 9P_FS
          SOUND SND SND_PCM SND_HDA_INTEL SND_HDA_GENERIC SND_VIRTIO
          UNIX98_PTYS POSIX_TIMERS SECCOMP SECCOMP_FILTER CIFS SMB_SERVER NLS_UTF8
          NAMESPACES USER_NS PID_NS NET_NS UTS_NS SYSVIPC IPC_NS NO_HZ_IDLE HIGH_RES_TIMERS
          ADVISE_SYSCALLS MEMBARRIER RSEQ"
fi
[ "$FLAVOUR" = pc ] && REQUIRED="$REQUIRED EFI EFI_STUB CMDLINE_BOOL X86_X2APIC
          DRM_VMWGFX SYSFB_SIMPLEFB DRM_SIMPLEDRM SERIO_I8042 KEYBOARD_ATKBD
          MOUSE_PS2 MOUSE_PS2_VMMOUSE USB USB_XHCI_HCD USB_EHCI_HCD USB_OHCI_HCD
          USB_UHCI_HCD HID_GENERIC USB_HID E1000 E1000E IGB IGC R8169 VMXNET3
          EFI_PARTITION EXT4_FS SATA_AHCI BLK_DEV_SD USB_STORAGE"
missing=0
for opt in $REQUIRED; do
    if ! grep -q "^CONFIG_$opt=y" "$OBJ/.config"; then
        echo "MISSING CONFIG_$opt"
        missing=1
    fi
done
[ "$FLAVOUR" != pc ] || grep -q "^CONFIG_INITRAMFS_SOURCE=\"$BASE/rosgd-initramfs.cpio\"$" .config ||
    { echo "MISSING CONFIG_INITRAMFS_SOURCE"; missing=1; }
grep -q '^CONFIG_DEFAULT_MMAP_MIN_ADDR=16384$' "$OBJ/.config" || { echo "MISSING CONFIG_DEFAULT_MMAP_MIN_ADDR=16384"; missing=1; }
# no module loader: every driver is in the Image
[ "$ARCH" != arm64 ] || ! grep -q '^CONFIG_MODULES=y' "$OBJ/.config" || { echo "UNWANTED CONFIG_MODULES"; missing=1; }
[ "$missing" -eq 0 ] || { echo "ROSGD-KERNEL-FAILED: required options dropped"; exit 1; }

echo "== building $TARGET with $(nproc) jobs"
$K -j"$(nproc)" $TARGET > "$OBJ/build.log" 2>&1 || {
    tail -30 "$OBJ/build.log"
    echo "ROSGD-KERNEL-FAILED: make"
    exit 1
}
cp "$OBJ/$IMAGE" "$OUT/$TARGET-$SUFFIX"
cp "$OBJ/.config" "$OUT/config-$SUFFIX"
ls -l "$OUT/$TARGET-$SUFFIX"
echo "ROSGD-KERNEL-DONE"
