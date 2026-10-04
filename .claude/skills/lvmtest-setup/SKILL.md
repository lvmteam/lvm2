---
name: lvmtest-setup
description: Set up host prerequisites for running lvmtest as a non-root user via session libvirt
trigger: /lvmtest-setup
---

# HOST PREREQUISITES (NON-ROOT USAGE)

lvmtest can run as a non-root user via session libvirt (`qemu:///session`).
Root and `libvirt` group members continue to use system libvirt (`qemu:///system`).

## Required

- Session libvirt: `qemu-kvm`, `libvirt-client`, `virt-install`, `qemu-img`, `genisoimage`
- Writable directories (session mode defaults):
  - `~/.local/share/libvirt/images` (VM disk images and snapshots)
  - `~/.cache/lvm-cluster-state` (cluster state files)
- A readable base OS cloud image (e.g. Fedora cloud qcow2)
  - Only have a non-cloud image (e.g. a generic install ISO/qcow2)? Run
    `./lvmtest image-prep -s <image>` to produce a cloud-init-capable copy
    (requires `virt-customize` from `guestfs-tools`; leaves the source untouched)
- Sufficient disk quota under `$HOME` for the cluster size

## Strongly recommended

- `kvm` group membership for `/dev/kvm` hardware acceleration:
  `sudo usermod -aG kvm $USER` (re-login required)
- Without KVM, VMs use slow software emulation and may hit boot timeouts

## One-time setup (non-root)

```bash
mkdir -p ~/.local/share/libvirt/images ~/.cache/lvm-cluster-state
# Copy or download a cloud image into the images directory
virsh net-start default   # session-scoped NAT network
```

## Example workflow (non-root)

```bash
./lvmtest -c my-cluster create
./lvmtest -i my-cluster group -g groups/shared-vg-3node-4scsi-caw-io2-512.txt
./lvmtest -i my-cluster destroy
```
