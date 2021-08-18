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

# Check that the LBS ($1) and PBS ($2) are accurately reported.
_check_env() {

	check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "$1"
	check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "$2"

	blockdev --getss "$dev1"
	blockdev --getpbsz "$dev1"
	blockdev --getss "$dev2"
	blockdev --getpbsz "$dev2"
}

#
# _run_test $BD1 $BD2 $type $optname "..."
#
# $BD1:    device to place the main LV on
# $BD2:    device to place the cache on
# $type    is cache or writecache to use in lvconvert --type $type
# $optname is either --cachevol or --cachepool to use in lvconvert
# "..." a sector size option to use in mkfs.xfs
#

_run_test() {
	vgcreate $SHARED $vg "$1"
	vgextend $vg "$2"
	lvcreate -n $lv1 -l 8 -an $vg "$1"
	lvcreate -n $lv2 -l 4 -an $vg "$2"
	lvchange -ay $vg/$lv1
	mkfs.xfs -f $5 "$DM_DEV_DIR/$vg/$lv1" |tee out
	_add_new_data_to_mnt
	lvconvert --yes --type $3 $4 $lv2 $vg/$lv1

	# TODO: check expected LBS of LV1
	# blockdev --getss "$DM_DEV_DIR/$vg/$lv1" |tee out
	# grep "$N" out
	# TODO: check expected PBS of LV1
	# blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1" |tee out
	# grep "$N" out

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

# Setup: dev1 LBS 512, PBS 4096  (using scsi-debug)
#        dev2 LBS 512, PBS 4096  (using scsi-debug)
#        dev3 LBS 512, PBS 512   (using loop)
#        dev4 LBS 512, PBS 512   (using loop)
#

aux prepare_scsi_debug_dev 256 sector_size=512 physblk_exp=3
aux prepare_devs 2 64

# Tests with fs block sizes require a libblkid version that shows BLOCK_SIZE
vgcreate $vg "$dev1"
lvcreate -n $lv1 -L50 $vg
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE || skip
lvchange -an $vg
vgremove -ff $vg

# loopa/loopb have LBS 512 PBS 512
which fallocate || skip
fallocate -l 64M loopa
fallocate -l 64M loopb

for i in {1..5}; do
	LOOP1=$(losetup -f loopa --show || true)
	test -n "$LOOP1" && break
done
for i in {1..5} ; do
	LOOP2=$(losetup -f loopb --show || true)
	test -n "$LOOP2" && break
done

# prepare devX mapping so it works for real & fake dev dir
d=3
for i in "$LOOP1" "$LOOP2"; do
	echo "$i"
	m=${i##*loop}
	test -e "$DM_DEV_DIR/loop$m" || mknod "$DM_DEV_DIR/loop$m" b 7 "$m"
	eval "dev$d=\"$DM_DEV_DIR/loop$m\""
	d=$(( d + 1 ))
done

# verify dev1/dev2 have LBS 512 PBS 4096
_check_env "512" "4096"

# verify dev3/dev4 have LBS 512 PBS 512
blockdev --getss "$LOOP1" | grep 512
blockdev --getss "$LOOP2" | grep 512
blockdev --getpbsz "$LOOP1" | grep 512
blockdev --getpbsz "$LOOP2" | grep 512

aux extend_filter "a|$dev3|" "a|$dev4|"
aux extend_devices "$dev3" "$dev4"

# place main LV on dev1 with LBS 512, PBS 4096
# and the cache on dev3 with LBS 512, PBS 512

_run_test "$dev1" "$dev3" "writecache" "--cachevol" ""
_run_test "$dev1" "$dev3" "cache" "--cachevol"  ""
_run_test "$dev1" "$dev3" "cache" "--cachepool" ""

# place main LV on dev3 with LBS 512, PBS 512
# and the cache on dev1 with LBS 512, PBS 4096

_run_test "$dev3" "$dev1" "writecache" "--cachevol" ""
_run_test "$dev3" "$dev1" "cache" "--cachevol"  ""
_run_test "$dev3" "$dev1" "cache" "--cachepool" ""

# place main LV on dev1 with LBS 512, PBS 4096
# and the cache on dev3 with LBS 512, PBS 512
# and force xfs sectsz 512

_run_test "$dev1" "$dev3" "writecache" "--cachevol" "-s size=512"
_run_test "$dev1" "$dev3" "cache" "--cachevol" "-s size=512"
_run_test "$dev1" "$dev3" "cache" "--cachepool" "-s size=512"

# place main LV on dev3 with LBS 512, PBS 512
# and the cache on dev1 with LBS 512, PBS 4096
# and force xfs sectsz 4096

_run_test "$dev3" "$dev1" "writecache" "--cachevol" "-s size=4096"
_run_test "$dev3" "$dev1" "cache" "--cachevol" "-s size=4096"
_run_test "$dev3" "$dev1" "cache" "--cachepool" "-s size=4096"


losetup -d "$LOOP1" || true
losetup -d "$LOOP2" || true
rm loopa loopb

aux cleanup_scsi_debug_dev
