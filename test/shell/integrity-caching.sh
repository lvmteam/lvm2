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

test "${LVM_VALGRIND:-0}" -eq 0 || skip # too slow test for valgrind
which mkfs.ext4 || skip
which resize2fs || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p $mnt

aux prepare_devs 9 80

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
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
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

	# The files written above are in the writecache so reading
	# them back later will come from the writecache and not from the
	# corrupted dev.  Write a bunch of new data to the fs to clear
	# the original files from the writecache, so when they are read
	# back the data will hopefully come from the underlying disk and
	# trigger reading the corrupted data.
	mkdir $mnt/new1
	cat randA > $mnt/new1/randA
	cat randB > $mnt/new1/randB
	cat randC > $mnt/new1/randC
	sync
	du -h $mnt/new1
	cp -r $mnt/new1 $mnt/new2 || true
	cp -r $mnt/new1 $mnt/new3 || true
	cp -r $mnt/new1 $mnt/new4 || true
	sync
	du -h $mnt
	df -h
	# hopefully fileA is no longer in the writecache.

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

# lv1 is a raid+integrity LV
# three variations of caching on lv1:
#
# 1. lvcreate --type cache-pool -n fast -l 4 $vg $dev6
#    lvconvert --type cache --cachepool fast $vg/$lv1
#
# 2. lvcreate --type linear -n fast -l 4 $vg $dev6
#    lvconvert --type cache --cachvol fast $vg/$lv1
#
# 3. lvcreate --type linear -n fast -l 4 $vg $dev6
#    lvconvert --type writecache --cachvol fast $vg/$lv1

do_test() {

# --cachepool | --cachevol
local create_type=$1

# cache | writecache
local convert_type=$2

# --type cache-pool | --type linear
local convert_option=$3

# corig | wcorig
local suffix=$4

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
_test_fs_with_read_repair "$dev1"
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_0 |tee mismatch
not grep ' 0 ' mismatch
lvs -o integritymismatches $vg/${lv1}_${suffix} |tee mismatch
not grep ' 0 ' mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvs -a -o name,size,segtype,devices,sync_percent $vg
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}
_test_fs_with_read_repair "$dev1" "$dev2"
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_0 |tee mismatch
not grep ' 0 ' mismatch
lvs -o integritymismatches $vg/${lv1}_${suffix} |tee mismatch
not grep ' 0 ' mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvconvert --splitcache $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}
_test_fs_with_read_repair "$dev1" "$dev2" "$dev3"
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_0
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_1
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_2
lvs -o integritymismatches $vg/${lv1}_${suffix} |tee mismatch
not grep ' 0 ' mismatch
lvconvert --splitcache $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing integrity from an active LV

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
_add_more_data_to_mnt
lvconvert --splitcache $vg/$lv1
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test adding integrity to an active LV

_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
# Can only be enabled while raid is top level lv (for now.)
not lvconvert --raidintegrity y $vg/${lv1}_${suffix}
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while inactive

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
umount $mnt
lvchange -an $vg/$lv1
# use two new devs for raid extend to ensure redundancy
vgextend $vg "$dev7" "$dev8"
lvs -a -o name,segtype,devices $vg
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8"
lvs -a -o name,segtype,devices $vg
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while active

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
# use two new devs for raid extend to ensure redundancy
vgextend $vg "$dev7" "$dev8"
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8"
lvs -a -o name,size,segtype,devices,sync_percent $vg
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
vgextend $vg "$dev7" "$dev8" "$dev9"
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8" "$dev9"
lvs -a -o name,size,segtype,devices,sync_percent $vg
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
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
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
# currently only allowed while raid is top level lv
not lvconvert -y -m+1 $vg/${lv1}_${suffix}
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
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
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
lvconvert -y -m-1 $vg/${lv1}_${suffix}
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test disallowed operations on raid+integrity

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
not lvconvert -y -m-1 $vg/$lv1
not lvconvert -y -m-1 $vg/${lv1}_${suffix}
not lvconvert --splitmirrors 1 -n tmp -y $vg/$lv1
not lvconvert --splitmirrors 1 -n tmp -y $vg/${lv1}_${suffix}
not lvconvert --splitmirrors 1 --trackchanges -y $vg/$lv1
not lvconvert --splitmirrors 1 --trackchanges -y $vg/${lv1}_${suffix}
not lvchange --syncaction repair $vg/$lv1
not lvchange --syncaction repair $vg/${lv1}_${suffix}
not lvreduce -L4M $vg/$lv1
not lvreduce -L4M $vg/${lv1}_${suffix}
not lvcreate -s -n snap -L4M $vg/${lv1}_${suffix}
# plan to enable snap on top level raid+integrity, so then
# snap+writecache+raid+integrity should be allowed.
not lvcreate -s -n snap -L4M $vg/$lv1
lvs -a -o name,size,segtype,devices
not pvmove -n $vg/$lv1 "$dev1"
not pvmove -n $vg/${lv1}_${suffix} "$dev1"
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
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
_test_fs_with_read_repair "$dev1"
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_0 |tee mismatch
not grep ' 0 ' mismatch
lvs -o integritymismatches $vg/${lv1}_${suffix} |tee mismatch
not grep ' 0 ' mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y --raidintegritymode bitmap -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}_rimage_3
aux wait_recalc $vg/${lv1}_${suffix}_rimage_4
aux wait_recalc $vg/${lv1}_${suffix}
_test_fs_with_read_repair "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_0
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_1
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_2
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_3
lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_4
lvs -o integritymismatches $vg/${lv1}_${suffix} |tee mismatch
not grep ' 0 ' mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvremove $vg/$lv1
vgremove -ff $vg

# remove from active lv
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
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
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg
}

do_test cache-pool cache --cachepool corig
do_test linear cache --cachevol corig
do_test linear writecache --cachevol wcorig

# TODO: add do_test() variant that skips adding the cache to lv1.
# This would be equivalent to integrity.sh which could be dropped.
