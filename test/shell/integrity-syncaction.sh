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

which mkfs.ext4 || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p $mnt

aux prepare_devs 3 40

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 4096) printf "B" ; while (z++ < 16384) printf "b" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

_prepare_vg() {
	# zero devs so we are sure to find the correct file data
	# on the underlying devs when corrupting it
	aux clear_devs "$dev1" "$dev2" "$dev3"
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"
	pvs
}

_test1() {
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# we don't want fileA to be located too early in the fs,
	# otherwise activating the LV will trigger the corruption
	# to be found and corrected, leaving nothing for syncaction
	# to find and correct.
	dd if=/dev/urandom of=$mnt/rand16M bs=1M count=16

	cp fileA $mnt
	cp fileB $mnt
	cp fileC $mnt

	umount $mnt
	lvchange -an $vg/$lv1

	aux corrupt_dev "$dev1" BBBBBBBBBBBBBBBBB BBBBBBBBCBBBBBBBB

	lvchange -ay $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	grep 0 mismatch

	lvchange --syncaction check $vg/$lv1

	aux wait_recalc $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	not grep 0 mismatch

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt
	cmp -b $mnt/fileA fileA
	cmp -b $mnt/fileB fileB
	cmp -b $mnt/fileC fileC
	umount $mnt
}

_test2() {
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# we don't want fileA to be located too early in the fs,
	# otherwise activating the LV will trigger the corruption
	# to be found and corrected, leaving nothing for syncaction
	# to find and correct.
	dd if=/dev/urandom of=$mnt/rand16M bs=1M count=16

	cp fileA $mnt
	cp fileB $mnt
	cp fileC $mnt

	umount $mnt
	lvchange -an $vg/$lv1

	# corrupt fileB and fileC on dev1
	aux corrupt_dev "$dev1" BBBBBBBBBBBBBBBBB BBBBBBBBCBBBBBBBB
	aux corrupt_dev "$dev1" CCCCCCCCCCCCCCCCC DDDDDDDDDDDDDDDDD

	# corrupt fileA on dev2
	aux corrupt_dev "$dev2" AAAAAAAAAAAAAAAAA AAAAAAAAAAAAAAEAA

	lvchange -ay $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	grep 0 mismatch
	lvs -o integritymismatches $vg/${lv1}_rimage_1 |tee mismatch
	grep 0 mismatch

	lvchange --syncaction check $vg/$lv1

	aux wait_recalc $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	not grep 0 mismatch
	lvs -o integritymismatches $vg/${lv1}_rimage_1 |tee mismatch
	not grep 0 mismatch

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt
	cmp -b $mnt/fileA fileA
	cmp -b $mnt/fileB fileB
	cmp -b $mnt/fileC fileC
	umount $mnt
}

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_test1
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_test2
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I 4K -l 6 $vg "$dev1" "$dev2" "$dev3"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_test1
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

