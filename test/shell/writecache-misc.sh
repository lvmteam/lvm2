#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test single lv cache options

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

. lib/inittest

mkfs_mount_umount()
{
        lvt=$1

        mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lvt"
        mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
        cp pattern1 "$mount_dir/pattern1"
        dd if=/dev/zero of="$mount_dir/zeros2M" bs=1M count=32 conv=fdatasync
        umount "$mount_dir"
}

mount_umount()
{
        lvt=$1

        mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
        diff pattern1 "$mount_dir/pattern1"
        dd if="$mount_dir/zeros2M" of=/dev/null bs=1M count=32
        umount "$mount_dir"
}

aux have_writecache 1 0 0 || skip
which mkfs.xfs || skip

mount_dir="mnt"
mkdir -p "$mount_dir"

# generate random data
dd if=/dev/urandom of=pattern1 bs=512K count=1

aux prepare_devs 4

vgcreate $vg "$dev1" "$dev2" "$dev3" "$dev4"


# Create writecache without a specified name so it gets automatic name
lvcreate -n $lv1 -l 4 -an $vg "$dev1"
lvcreate -y --type writecache -l 4 --cachevol $lv1 $vg "$dev2"
check lv_exists $vg lvol0
lvremove -y $vg

#
# Test pvmove with writecache
#

lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev4"
lvcreate -n $lv2 -l 4 -an $vg "$dev2"

lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1

lvchange -ay $vg/$lv1
mkfs_mount_umount $lv1

# cannot pvmove the cachevol
not pvmove "$dev2" "$dev3"

# can pvmove the origin
pvmove "$dev1" "$dev3"

mount_umount $lv1

# can pvmove the origin, naming the lv with the writecache
pvmove -n $vg/$lv1 "$dev3" "$dev1"

mount_umount $lv1
lvchange -an $vg/$lv1
lvremove -y $vg/$lv1


#
# Test partial and degraded activation
#

lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev2"
lvcreate -n $lv2 -l 16 -an $vg "$dev3" "$dev4"

lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1
lvs -a -o+devices $vg
lvchange -an $vg/$lv1

aux hide_dev "$dev1"
not lvchange -ay $vg/$lv1
not lvchange -ay --partial $vg/$lv1
not lvchange -ay --activationmode degraded $vg/$lv1
aux unhide_dev "$dev1"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1

aux hide_dev "$dev3"
not lvchange -ay $vg/$lv1
not lvchange -ay --partial $vg/$lv1
not lvchange -ay --activationmode degraded $vg/$lv1
aux unhide_dev "$dev3"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1

vgremove -ff $vg
