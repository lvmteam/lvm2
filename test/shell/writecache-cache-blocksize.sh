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

# Test dm-writecache and dm-cache with different block size combinations

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_writecache 1 0 0 || skip
which mkfs.xfs || skip

mnt="mnt"
mkdir -p $mnt

awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_add_new_data_to_mnt() {
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
	sync
}

_add_more_data_to_mnt() {
	mkdir $mnt/more
	cp fileA $mnt/more
	cp fileB $mnt/more
	cp fileC $mnt/more
	cp randA $mnt/more
	cp randB $mnt/more
	cp randC $mnt/more
	sync
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

_verify_more_data_on_mnt() {
	diff randA $mnt/more/randA
	diff randB $mnt/more/randB
	diff randC $mnt/more/randC
	diff fileA $mnt/more/fileA
	diff fileB $mnt/more/fileB
	diff fileC $mnt/more/fileC
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

# Check that the LBS/PBS that were set up is accurately reported for the devs.
_check_env() {

	check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "$1"
	check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "$2"

	blockdev --getss "$dev1"
	blockdev --getpbsz "$dev1"
	blockdev --getss "$dev2"
	blockdev --getpbsz "$dev2"
}

#
# _run_test $BS1 $BS2 $type $optname "..."
#
# $BS1:    the xfs sectsz is verified to match $BS1, after mkfs
# $BS2:    the lv1 LBS is verified to match $BS2, after cache is added to lv1
# $type    is cache or writecache to use in lvconvert --type $type
# $optname is either --cachevol or --cachepool to use in lvconvert
# "..." a sector size option to use in mkfs.xfs
#

_run_test() {
	vgcreate $SHARED $vg "$dev1"
	vgextend $vg "$dev2"
	lvcreate -n $lv1 -l 8 -an $vg "$dev1"
	lvcreate -n $lv2 -l 4 -an $vg "$dev2"
	lvchange -ay $vg/$lv1
	mkfs.xfs -f $5 "$DM_DEV_DIR/$vg/$lv1" |tee out
	grep "sectsz=$1" out
	_add_new_data_to_mnt
	lvconvert --yes --type $3 $4 $lv2 $vg/$lv1
	blockdev --getss "$DM_DEV_DIR/$vg/$lv1" |tee out
	grep "$2" out
	blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
	_add_more_data_to_mnt
	_verify_data_on_mnt
	lvconvert --splitcache $vg/$lv1
	check lv_field $vg/$lv1 segtype linear
	blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
	blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
	_verify_data_on_mnt
	_verify_more_data_on_mnt
	umount $mnt
	lvchange -an $vg/$lv1
	lvchange -an $vg/$lv2
	_verify_data_on_lv
	lvremove $vg/$lv1
	lvremove $vg/$lv2
	vgremove $vg
}

# Setup: LBS 512, PBS 512
aux prepare_scsi_debug_dev 256 || skip
aux prepare_devs 2 64

# Tests with fs block sizes require a libblkid version that shows BLOCK_SIZE
vgcreate $vg "$dev1"
lvcreate -n $lv1 -L50 $vg
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE || skip
lvchange -an $vg
vgremove -ff $vg

_check_env "512" "512"

# lbs 512, pbs 512, xfs 512, wc bs 512
_run_test 512 512 "writecache" "--cachevol" ""
# lbs 512, pbs 512, xfs 512, cache bs 512
_run_test 512 512 "cache" "--cachevol"  ""
_run_test 512 512 "cache" "--cachepool" ""

# lbs 512, pbs 512, xfs -s 4096, wc bs 4096
_run_test 4096 4096 "writecache" "--cachevol" "-s size=4096"
# lbs 512, pbs 512, xfs -s 4096, cache bs 512
_run_test 4096 512 "cache" "--cachevol"  "-s size=4096"
_run_test 4096 512 "cache" "--cachepool" "-s size=4096"

aux cleanup_scsi_debug_dev


# Setup: LBS 512, PBS 4096
aux prepare_scsi_debug_dev 256 sector_size=512 physblk_exp=3
aux prepare_devs 2 64

_check_env "512" "4096"

# lbs 512, pbs 4k, xfs 4k, wc bs 4k
_run_test 4096 4096 "writecache" "--cachevol" ""
# lbs 512, pbs 4k, xfs 4k, cache bs 512
_run_test 4096 512 "cache" "--cachevol"  ""
_run_test 4096 512 "cache" "--cachepool" ""

# lbs 512, pbs 4k, xfs -s 512, wc bs 512
_run_test 512 512 "writecache" "--cachevol" "-s size=512"
# lbs 512, pbs 4k, xfs -s 512, cache bs 512
_run_test 512 512 "cache" "--cachevol"  "-s size=512"
_run_test 512 512 "cache" "--cachepool" "-s size=512"

aux cleanup_scsi_debug_dev


# Setup: LBS 4096, PBS 4096
aux prepare_scsi_debug_dev 256 sector_size=4096
aux prepare_devs 2 64

_check_env "4096" "4096"

# lbs 4k, pbs 4k, xfs 4k, wc bs 4k
_run_test 4096 4096 "writecache" "--cachevol" ""
# lbs 4k, pbs 4k, xfs 4k, cache bs 4k
_run_test 4096 4096 "cache" "--cachevol"  ""
_run_test 4096 4096 "cache" "--cachepool" ""

aux cleanup_scsi_debug_dev


# Setup: LBS 512, PBS 512
aux prepare_scsi_debug_dev 256 || skip
aux prepare_devs 2 64

_check_env "512" "512"

vgcreate $SHARED $vg "$dev1"
vgextend $vg "$dev2"
lvcreate -n $lv1 -l 8 -an $vg "$dev1"
lvcreate -n $lv2 -l 4 -an $vg "$dev2"
lvconvert --yes --type writecache --cachevol $lv2 --cachesettings "block_size=4096" $vg/$lv1
lvs -o writecacheblocksize $vg/$lv1 |tee out
grep 4096 out
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1" |tee out
grep "sectsz=4096" out
_add_new_data_to_mnt
blockdev --getss "$DM_DEV_DIR/$vg/$lv1" |tee out
grep 4096 out
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
_add_more_data_to_mnt
_verify_data_on_mnt
lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
_verify_data_on_mnt
_verify_more_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
lvchange -an $vg/$lv2
_verify_data_on_lv
lvremove $vg/$lv1
lvremove $vg/$lv2
vgremove $vg

aux cleanup_scsi_debug_dev

