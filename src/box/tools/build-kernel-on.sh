#!/bin/sh
# Build a kernel on the Intel Mac's Linux VM, and bring it back.
#
#   tools/build-kernel-on.sh [flavour] [host]
#
# flavour is build-kernel.sh's: pc (default, the PC image's), box, or
# box-aarch64.  host is the Mac with VMware Fusion and the build VM that
# tools/make-build-vm.sh made -- macpro by default, over the Thunderbolt
# bridge; the VM itself is reached through it.
#
# A Linux kernel does not build on macOS, so it is built in a Linux VM.  On
# an Apple Silicon Mac that VM is emulated and the PC kernel, which has
# every graphics driver in it, takes 47 minutes; on the Intel Mac it runs
# on the processor itself and takes 2m43s.  So the ROM is made here, where
# zig cross-compiles it in a minute, and only the kernel goes across.
#
# The VM keeps its own kernel tree, so a second build of the same flavour
# only compiles what changed -- a new ROM relinks in seconds.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
flavour=${1:-pc}
host=${2:-macpro}
vm_dir=${ROSGD_BUILD_VM_DIR:-/Volumes/U/oberon/rosgd-buildvm}
vmx=$vm_dir/rosgd-build.vmwarevm/rosgd-build.vmx
vmrun="/Applications/VMware Fusion.app/Contents/Public/vmrun"

echo "== $host: where the build VM is"
ssh "$host" "'$vmrun' -T fusion list | grep -q rosgd-build || '$vmrun' -T fusion start '$vmx' nogui" >/dev/null 2>&1 || true
ip=$(ssh "$host" "mac=\$(awk -F'\"' '/^ethernet0.generatedAddress /{print tolower(\$2)}' '$vmx');
     awk -v m=\"\$mac\" '/^lease /{ip=\$2} /hardware ethernet/{h=tolower(\$3); sub(/;/,\"\",h);
     if (h==m) last=ip} END{print last}' /var/db/vmware/vmnet-dhcpd-vmnet8.leases")
[ -n "$ip" ] || { echo "no build VM on $host: tools/make-build-vm.sh makes one" >&2; exit 1; }
vm="build@$ip"
ssh_vm="ssh -J $host -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
echo "   $vm"

if [ "$flavour" = pc ]; then
    echo "== the ROM, here"
    make -C "$here" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null
fi

echo "== what it needs, over the bridge"
$ssh_vm "$vm" 'mkdir -p /mnt/rosgd/kernel /mnt/rosgd/.cache/pc /mnt/rosgd/.cache/out /mnt/build'
rsync -a -e "$ssh_vm" "$here/kernel/" "$vm:/mnt/rosgd/kernel/"
tar=$(ls "$here"/.cache/linux-*.tar.xz 2>/dev/null | head -1)
[ -z "$tar" ] || $ssh_vm "$vm" "test -f /mnt/rosgd/.cache/$(basename "$tar")" ||
    rsync -a -e "$ssh_vm" "$tar" "$vm:/mnt/rosgd/.cache/"
[ "$flavour" != pc ] ||
    rsync -a -e "$ssh_vm" "$here/build/initramfs.cpio" "$vm:/mnt/rosgd/.cache/pc/initramfs.cpio"

echo "== the kernel, on the processor itself"
$ssh_vm "$vm" "cd /mnt/rosgd && sh kernel/build-kernel.sh $flavour" | tail -3

echo "== back here"
mkdir -p "$here/.cache/out"
rsync -a -e "$ssh_vm" "$vm:/mnt/rosgd/.cache/out/" "$here/.cache/out/"
ls -l "$here/.cache/out/" | tail -3
