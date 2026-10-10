#!/bin/sh
# Make the Linux build VM on a Mac with VMware Fusion.
#
#   tools/make-build-vm.sh [key.pub]...
#
# Run it on that Mac (or `ssh <mac> sh -s < tools/make-build-vm.sh`).  It
# makes a VM that builds Linux kernels for the box: an Ubuntu cloud image
# imported with ovftool, set up by cloud-init from a seed it writes here --
# the build user, the keys that may log in (this Mac's, and any given) and
# the packages kernel/build-kernel.sh wants.  16 GB and 20 processors, an
# 80 GB disc, NAT networking; it runs headless and stays up.
#
# Why a VM and not the processor itself: a Linux kernel does not build on
# macOS.  Why Fusion and not QEMU: Homebrew will not install a QEMU on an
# Intel Mac running macOS 26, and Fusion is there already.
#
# Afterwards tools/build-kernel-on.sh uses it: 2m43s for the PC kernel with
# every graphics driver in it, against 47 minutes emulated on an Apple
# Silicon Mac.
set -eu
here=${ROSGD_BUILD_VM_DIR:-$HOME/rosgd-buildvm}
image=${ROSGD_CLOUD_IMAGE:-https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.ova}
fusion="/Applications/VMware Fusion.app/Contents"
ovftool="$fusion/Library/VMware OVF Tool/ovftool"
vmrun="$fusion/Public/vmrun"
vdisk="$fusion/Library/vmware-vdiskmanager"
[ -x "$ovftool" ] || { echo "no VMware Fusion here ($ovftool)" >&2; exit 1; }

mkdir -p "$here/seed"
cd "$here"
[ -f noble.ova ] || { echo "== the cloud image"; curl -sSLo noble.ova "$image"; }

echo "== the seed: the build user, the keys, the packages"
keys=$(cat "$HOME"/.ssh/id_*.pub "$@" 2>/dev/null | sed 's/^/      - /')
cat > seed/meta-data <<EOM
instance-id: rosgd-build-1
local-hostname: rosgd-build
EOM
cat > seed/user-data <<EOM
#cloud-config
hostname: rosgd-build
users:
  - name: build
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    lock_passwd: true
    ssh_authorized_keys:
$keys
package_update: true
packages: [build-essential, bison, flex, libelf-dev, libssl-dev, bc, perl, cpio,
           e2fsprogs, python3, xz-utils, rsync, kmod, zstd, open-vm-tools]
growpart: {mode: auto, devices: ['/']}
resize_rootfs: true
runcmd:
  - [mkdir, -p, /mnt/rosgd, /mnt/build]
  - [chown, 'build:build', /mnt/rosgd, /mnt/build]
EOM
rm -f seed.iso
hdiutil makehybrid -iso -joliet -default-volume-name CIDATA -o seed.iso seed >/dev/null

if [ ! -d rosgd-build.vmwarevm ]; then
    echo "== importing it"
    "$ovftool" --acceptAllEulas --name=rosgd-build noble.ova . 2>&1 | tail -1
    "$vdisk" -x 80GB rosgd-build.vmwarevm/rosgd-build-disk1.vmdk >/dev/null 2>&1 || true
fi

echo "== 16 GB, 20 processors, the seed in its drive"
python3 - "$here/rosgd-build.vmwarevm/rosgd-build.vmx" "$here/seed.iso" <<'PY'
import re, sys
vmx, seed = sys.argv[1], sys.argv[2]
s = open(vmx).read()
def setkey(s, k, v):
    if re.search(rf"^{re.escape(k)} = ", s, re.M):
        return re.sub(rf"^{re.escape(k)} = .*$", f'{k} = "{v}"', s, flags=re.M)
    return s.rstrip("\n") + f'\n{k} = "{v}"\n'
for k, v in [("memsize", "16384"), ("numvcpus", "20"), ("cpuid.coresPerSocket", "20"),
             ("ethernet0.present", "TRUE"), ("ethernet0.connectionType", "nat"),
             ("ide1:0.present", "TRUE"), ("ide1:0.deviceType", "cdrom-image"),
             ("ide1:0.fileName", seed), ("ide1:0.startConnected", "TRUE"),
             ("tools.syncTime", "TRUE")]:
    s = setkey(s, k, v)
open(vmx, "w").write(s)
PY

echo "== starting it"
"$vmrun" -T fusion start "$here/rosgd-build.vmwarevm/rosgd-build.vmx" nogui >/dev/null 2>&1 || true
vmx=$here/rosgd-build.vmwarevm/rosgd-build.vmx
for i in $(seq 1 60); do
    mac=$(awk -F'"' '/^ethernet0.generatedAddress /{print tolower($2)}' "$vmx")
    ip=$(awk -v m="$mac" '/^lease /{ip=$2} /hardware ethernet/{h=tolower($3); sub(/;/,"",h);
         if (h==m) last=ip} END{print last}' /var/db/vmware/vmnet-dhcpd-vmnet8.leases 2>/dev/null)
    [ -n "$ip" ] && ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 "build@$ip" \
        'cloud-init status' 2>/dev/null | grep -q done && break
    sleep 10
done
echo "$ip" > "$here/address"
echo "the build VM is $ip ($(ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null "build@$ip" 'nproc' 2>/dev/null) processors)"
