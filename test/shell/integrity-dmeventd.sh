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

aux prepare_devs 6 64

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
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4"
	pvs
}

_add_new_data_to_mnt() {
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

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
	diff randA $mnt/randA
	diff randB $mnt/randB
	diff randC $mnt/randC
	diff fileA $mnt/1/fileA
	diff fileB $mnt/1/fileB
	diff fileC $mnt/1/fileC
	diff fileA $mnt/2/fileA
	diff fileB $mnt/2/fileB
	diff fileC $mnt/2/fileC
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

aux lvmconf \
        'activation/raid_fault_policy = "allocate"'

aux prepare_dmeventd

# raid1, one device fails, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4"
lvcreate --type raid1 -m 2 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_for_sync $vg $lv1
_add_new_data_to_mnt

aux disable_dev "$dev2"

# wait for dmeventd to call lvconvert --repair which should
# replace dev2 with dev4
sync
sleep 5

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev4" out

_add_more_data_to_mnt
_verify_data_on_mnt

aux enable_dev "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev4" out
grep "$dev1" out
grep "$dev3" out

umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# raid1, two devices fail, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type raid1 -m 2 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_for_sync $vg $lv1
_add_new_data_to_mnt

aux disable_dev "$dev1" "$dev2"

# wait for dmeventd to call lvconvert --repair which should
# replace dev1 and dev2 with dev4 and dev5
sync
sleep 5

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev4" out
grep "$dev5" out
grep "$dev3" out

_add_more_data_to_mnt
_verify_data_on_mnt

aux enable_dev "$dev1" "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev4" out
grep "$dev5" out
grep "$dev3" out

umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# raid6, one device fails, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
lvcreate --type raid6 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_for_sync $vg $lv1
_add_new_data_to_mnt

aux disable_dev "$dev2"

# wait for dmeventd to call lvconvert --repair which should
# replace dev2 with dev6
sync
sleep 5

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev6" out

_add_more_data_to_mnt
_verify_data_on_mnt

aux enable_dev "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev6" out

umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# raid10, one device fails, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type raid10 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_for_sync $vg $lv1
_add_new_data_to_mnt

aux disable_dev "$dev1"

# wait for dmeventd to call lvconvert --repair which should
# replace dev1 with dev5
sync
sleep 5

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
grep "$dev5" out

_add_more_data_to_mnt
_verify_data_on_mnt

aux enable_dev "$dev1"

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
grep "$dev5" out

umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg
