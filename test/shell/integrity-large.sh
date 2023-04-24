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

# Test writecache usage

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LOW_SPACE=1100

. lib/inittest

aux have_integrity 1 5 0 || skip
which mkfs.xfs || skip

mnt="mnt"
mkdir -p $mnt

# raid1 LV needs to be extended to 512MB to test imeta being exended
aux prepare_devs 4 632

# this test may consume lot of disk space - so make sure cleaning works
# also in failure case
cleanup_mounted_and_teardown()
{
	umount "$mnt" 2>/dev/null || true
	# Comment out this 'vgremove' when there is any need to analyze
	# content of the failed test dir, otherwise all is deleted.
	vgremove -ff $vg || true
	aux teardown
}

trap 'cleanup_mounted_and_teardown' EXIT

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_prepare_vg() {
	vgcreate $SHARED $vg "$dev1" "$dev2"
	pvs
}

_add_data_to_lv() {
	mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"

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

	umount $mnt
}

_verify_data_on_lv() {
	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	diff randA $mnt/randA
	diff randB $mnt/randB
	diff randC $mnt/randC
	diff fileA $mnt/1/fileA
	diff fileB $mnt/1/fileB
	diff fileC $mnt/1/fileC
	diff fileA $mnt/2/fileA
	diff fileB $mnt/2/fileB
	diff fileC $mnt/2/fileC

	umount $mnt
}

# lvextend to 512MB is needed for the imeta LV to
# be extended from 4MB to 8MB.

_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -L 300 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
_add_data_to_lv
# lv needs to be inactive when adding integrity to increase LBS from 512 and get a ribs of 4k
lvchange -an $vg/$lv1
lvconvert --raidintegrity y $vg/$lv1
lvchange -ay $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
_verify_data_on_lv
lvchange -an $vg/$lv1
lvextend -L 512M $vg/$lv1
lvs -a -o+devices $vg
lvchange -ay $vg/$lv1
_verify_data_on_lv
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
check lv_field $vg/${lv1}_rimage_0_imeta size "12.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "12.00m"

# provide space to extend the images onto new devs
vgextend $vg "$dev3" "$dev4"

# extending the images is possible using dev3,dev4
# but extending imeta on the existing dev1,dev2 fails
not lvextend -L +512M $vg/$lv1

# removing integrity will permit extending the images
# using dev3,dev4 since imeta limitation is gone
lvconvert --raidintegrity n $vg/$lv1
lvextend -L +512M $vg/$lv1
lvs -a -o+devices $vg

# adding integrity again will allocate new 12MB imeta LVs
# on dev3,dev4
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
check lv_field $vg/${lv1}_rimage_0_imeta size "20.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "20.00m"

lvchange -an $vg/$lv1
lvremove $vg/$lv1

# As the test doesn't wait for full resync
# delay legs so not all data need to be written.
aux delay_dev "$dev1" 1000 0 "$(( $(get first_extent_sector "$dev1") + 16000 )):1200000"
aux delay_dev "$dev2" 0 10 "$(( $(get first_extent_sector "$dev2") + 16000 )):1200000"


# this succeeds because dev1,dev2 can hold rmeta+rimage
lvcreate --type raid1 -n $lv1 -L 592M -an $vg "$dev1" "$dev2"
lvs -a -o+devices $vg
lvremove $vg/$lv1

# this fails because dev1,dev2 can hold rmeta+rimage, but not imeta
# and we require imeta to be on same devs as rmeta/rimeta
not lvcreate --type raid1 --raidintegrity y -n $lv1 -L 624M -an $vg "$dev1" "$dev2"
lvs -a -o+devices $vg

# this can allocate from more devs so there's enough space for imeta to
# be allocated in the vg, but lvcreate fails because rmeta+rimage are
# allocated from dev1,dev2, we restrict imeta to being allocated on the
# same devs as rmeta/rimage, and dev1,dev2 can't fit imeta.
not lvcreate --type raid1 --raidintegrity y -n $lv1 -L 624M -an $vg
lvs -a -o+devices $vg

# counterintuitively, increasing the size will allow lvcreate to succeed
# because rmeta+rimage are pushed to being allocated on dev1,dev2,dev3,dev4
# which means imeta is now free to be allocated from dev3,dev4 which have
# plenty of space
lvcreate --type raid1 --raidintegrity y -n $lv1 -L 640M -an $vg
lvs -a -o+devices $vg

vgremove -ff $vg
