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


. lib/inittest --skip-with-lvmpolld

test "${LVM_VALGRIND:-0}" -eq 0 || skip # too slow test for valgrind
which mkfs.ext4 || skip
which resize2fs || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p $mnt

aux prepare_devs 5 64

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 4096) printf "B" ; while (z++ < 16384) printf "b" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_prepare_vg() {
	# zero devs so we are sure to find the correct file data
	# on the underlying devs when corrupting it
	aux clear_devs "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	pvs
}

_test_fs_with_read_repair() {
	mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	cp randA $mnt
	cp randB $mnt
	cp randC $mnt
	cp fileA $mnt
	cp fileB $mnt
	cp fileC $mnt

	umount $mnt
	lvchange -an $vg/$lv1

	for dev in "$@"; do
		aux corrupt_dev "$dev" BBBBBBBBBBBBBBBBB BBBBBBBBCBBBBBBBB
	done

	lvchange -ay $vg/$lv1

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt
	cmp -b $mnt/fileA fileA
	cmp -b $mnt/fileB fileB
	cmp -b $mnt/fileC fileC
	umount $mnt
}

_add_new_data_to_mnt() {
	mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# add original data
	cp randA $mnt
	cp randB $mnt
	cp randC $mnt
	mkdir $mnt/1
	cp fileA $mnt/1
	cp fileB $mnt/1
	cp fileC $mnt/1
	mkdir $mnt/2
	cp fileA $mnt/2
	cp fileB $mnt/2
	cp fileC $mnt/2
}

_add_more_data_to_mnt() {
	mkdir $mnt/more
	cp fileA $mnt/more
	cp fileB $mnt/more
	cp fileC $mnt/more
	cp randA $mnt/more
	cp randB $mnt/more
	cp randC $mnt/more
}

_verify_data_on_mnt() {
	cmp -b randA $mnt/randA
	cmp -b randB $mnt/randB
	cmp -b randC $mnt/randC
	cmp -b fileA $mnt/1/fileA
	cmp -b fileB $mnt/1/fileB
	cmp -b fileC $mnt/1/fileC
	cmp -b fileA $mnt/2/fileA
	cmp -b fileB $mnt/2/fileB
	cmp -b fileC $mnt/2/fileC
}

_verify_data_on_lv() {
	lvchange -ay $vg/$lv1
	mount "$DM_DEV_DIR/$vg/$lv1" $mnt
	_verify_data_on_mnt
	rm $mnt/randA
	rm $mnt/randB
	rm $mnt/randC
	rm -rf $mnt/1
	rm -rf $mnt/2
	umount $mnt
	lvchange -an $vg/$lv1
}

# Test corrupting data on an image and verifying that
# it is detected by integrity and corrected by raid.

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1"
lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
not grep 0 mismatch
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1" "$dev2"
lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
not grep 0 mismatch
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid4 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1" "$dev2" "$dev3"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1" "$dev2" "$dev3"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/${lv1}_rimage_3
lvs -o integritymismatches $vg/${lv1}_rimage_4
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid10 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1" "$dev3"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/${lv1}_rimage_3
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing integrity from an active LV

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid4 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid10 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test adding integrity to an active LV

_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid4 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid10 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while inactive

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
umount $mnt
lvchange -an $vg/$lv1
lvextend -l 16 $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o name,segtype,devices,sync_percent $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,sync_percent,devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
umount $mnt
lvchange -an $vg/$lv1
lvextend -l 16 $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o name,segtype,devices,sync_percent $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while active

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid10 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test adding image to raid1

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
_add_new_data_to_mnt
lvconvert -y -m+1 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
lvs -a -o+devices $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing image from raid1

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
lvconvert -y -m-1 $vg/$lv1
lvs -a -o+devices $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test disallowed operations on raid+integrity

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
not lvconvert -y -m-1 $vg/$lv1
not lvconvert --splitmirrors 1 -n tmp -y $vg/$lv1
not lvconvert --splitmirrors 1 --trackchanges -y $vg/$lv1
not lvchange --syncaction repair $vg/$lv1
not lvreduce -L4M $vg/$lv1
not pvmove -n $vg/$lv1 "$dev1"
not pvmove "$dev1"
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Repeat many of the tests above using bitmap mode

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1"
lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
not grep 0 mismatch
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y --raidintegritymode bitmap -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/${lv1}_rimage_3
lvs -o integritymismatches $vg/${lv1}_rimage_4
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# remove from active lv
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# add to active lv
_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
_add_new_data_to_mnt
lvconvert --raidintegrity y --raidintegritymode bitmap $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# lvextend active
_prepare_vg
lvcreate --type raid1 --raidintegrity y --raidintegritymode bitmap -m1 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# add image to raid1
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
_add_new_data_to_mnt
lvconvert -y -m+1 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test that raid+integrity cannot be a sublv
# part1: cannot add integrity to a raid LV that is already a sublv

_prepare_vg

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvconvert -y --type thin-pool $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_tdata
not lvconvert --raidintegrity y $vg/${lv1}_tmeta
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvconvert -y --type cache-pool $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_cdata
not lvconvert --raidintegrity y $vg/${lv1}_cmeta
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type cache-pool -n cpool -l 8 $vg
lvconvert -y --type cache --cachepool cpool $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type raid1 -m1 -n cvol -l 8 $vg
lvconvert -y --type cache --cachevol cvol $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
not lvconvert --raidintegrity y $vg/cvol
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate -n cvol -l 8 $vg
lvchange -an $vg
lvconvert -y --type writecache --cachevol cvol $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_wcorig
lvremove -y $vg/$lv1

# Test that raid+integrity cannot be a sublv
# part2: cannot convert an existing raid+integrity LV into a sublv

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvconvert -y --type thin-pool $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_tdata
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type raid1 -m1 -n $lv2 -l 8 $vg
lvconvert -y --type cache --cachevol $lv2 $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
not lvconvert --raidintegrity y $vg/${lv2}_cvol
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type raid1 -m1 -n $lv2 -l 8 $vg
lvconvert -y --type cache --cachepool $lv2 $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
not lvconvert --raidintegrity y $vg/${lv2}_cpool_cdata
not lvconvert --raidintegrity y $vg/${lv2}_cpool_cmeta
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 --raidintegrity y --integritysettings journal_watermark=10 $vg
lvs -a -o integritysettings $vg/${lv1}_rimage_0 | grep journal_watermark=10
not lvchange --integritysettings journal_watermark=20 $vg/$lv1
lvchange -an $vg/$lv1
lvchange --integritysettings journal_watermark=80 $vg/$lv1
lvchange -ay $vg/$lv1
lvs -a -o integritysettings $vg/${lv1}_rimage_0 | grep journal_watermark=80
# The journal_watermark value reported in the table may be
# slightly different from the value set due to the kernel
# reporting it as a computed value that may include rounding.
dmsetup table $vg-${lv1}_rimage_0 | tee out
grep journal_watermark out
lvchange -an $vg/$lv1
lvremove -y $vg/$lv1

vgremove -ff $vg
