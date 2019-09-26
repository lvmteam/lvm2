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

. lib/inittest

mkfs_mount_umount()
{
        lvt=$1

        lvchange -ay $vg/$lvt

        mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lvt"
        mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
        cp pattern1 "$mount_dir/pattern1"
        dd if=/dev/zero of="$mount_dir/zeros2M" bs=1M count=32 conv=fdatasync
        umount "$mount_dir"

        lvchange -an $vg/$lvt
}

mount_umount()
{
        lvt=$1

        lvchange -ay $vg/$lvt

        mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
        diff pattern1 "$mount_dir/pattern1"
        dd if="$mount_dir/zeros2M" of=/dev/null bs=1M count=32
        umount "$mount_dir"

        lvchange -an $vg/$lvt
}

aux have_writecache 1 0 0 || skip
which mkfs.xfs || skip

mount_dir="mnt"
mkdir -p "$mount_dir"

# generate random data
dd if=/dev/urandom of=pattern1 bs=512K count=1

aux prepare_devs 4

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4"

lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev4"
lvcreate -n $lv2 -l 4 -an $vg "$dev2"

#
# split when no devs are missing
#

lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1

mkfs_mount_umount $lv1

lvconvert --splitcache $vg/$lv1
lvs -o segtype $vg/$lv1 | grep linear
lvs -o segtype $vg/$lv2 | grep linear

mount_umount $lv1

#
# split while cachevol is missing
#

lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1

mkfs_mount_umount $lv1

aux disable_dev "$dev2"

not lvconvert --splitcache $vg/$lv1
lvconvert --splitcache --force --yes $vg/$lv1

lvs -o segtype $vg/$lv1 | grep linear

aux enable_dev "$dev2"
lvs -o segtype $vg/$lv2 | grep linear

vgck --updatemetadata $vg
lvs $vg
vgchange -an $vg
vgextend --restoremissing $vg "$dev2"


#
# split while cachevol has 1 of 2 PVs
#

lvremove $vg/$lv2
lvcreate -n $lv2 -l 14 -an $vg "$dev2" "$dev3"

lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1

mkfs_mount_umount $lv1

aux disable_dev "$dev3"

not lvconvert --splitcache $vg/$lv1
lvconvert --splitcache --force --yes $vg/$lv1

lvs -o segtype $vg/$lv1 | grep linear

aux enable_dev "$dev3"
lvs -o segtype $vg/$lv2 | grep linear

vgck --updatemetadata $vg
lvs $vg
vgchange -an $vg
vgextend --restoremissing $vg "$dev3"

vgremove -ff $vg
