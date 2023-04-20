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

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p $mnt

# prepare_devs uses ramdisk backing which has 512 LBS and 4K PBS
# This should cause mkfs.xfs to use 4K sector size,
# and integrity to use 4K block size
aux prepare_devs 2 64

vgcreate $vg "$dev1" "$dev2"
blockdev --getss "$dev1"
blockdev --getpbsz "$dev1"
blockdev --getss "$dev2"
blockdev --getpbsz "$dev2"

# add integrity while LV is inactive
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvconvert --raidintegrity y $vg/$lv1
lvchange -ay $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs unmounted
# lvconvert will use ribs 512 to avoid increasing LBS from 512 to 4k on active LV
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvchange -ay $vg
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs mounted
# lvconvert will use ribs 512 to avoid increasing LBS from 512 to 4k on active LV
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

vgremove -ff $vg
