#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test basic VDO lifecycle: create filesystem, write data, stop/start
# the VDO device, and verify data survives the restart.
# Derived from VDOTest::Basic01 in vdo-devel.

. lib/inittest --skip-with-lvmpolld --with-extended

aux have_vdo 6 2 0 || skip
which mkfs.ext4 || skip

aux prepare_vg 1 6400

lvcreate --vdo -L5G -V10G -n $lv1 $vg/vdopool

mkfs.ext4 -E nodiscard "$DM_DEV_DIR/$vg/$lv1"

mount_dir="mnt_vdo"
mkdir -p "$mount_dir"
mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

echo "Hello World" > "$mount_dir/foo1"
mkdir -p "$mount_dir/dir2"
cp "$mount_dir/foo1" "$mount_dir/dir2/foo2"
cp "$mount_dir/foo1" "$mount_dir/foo3"

sync
echo 3 > /proc/sys/vm/drop_caches

grep -q "Hello World" "$mount_dir/foo1"
grep -q "Hello World" "$mount_dir/dir2/foo2"

umount "$mount_dir"

# Deactivate and reactivate VDO to exercise clean stop/start
lvchange -an $vg/$lv1
lvchange -an $vg/vdopool

lvchange -ay $vg/vdopool
lvchange -ay $vg/$lv1

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

grep -q "Hello World" "$mount_dir/foo3"

umount "$mount_dir"
vgremove -ff $vg
