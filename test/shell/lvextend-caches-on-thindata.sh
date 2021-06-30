#!/usr/bin/env bash

# Copyright (C) 2017-2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

# lvextend thin pool data that has cache|writecache attached

. lib/inittest

do_test()
{
	local tp=$1
	local lvt=$2

	# create some initial data
	lvchange -ay $vg/$lvt
	mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lvt"
	mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
	cp pattern "$mount_dir/pattern1"
	dd if=/dev/urandom of="$mount_dir/rand100M" bs=1M count=100 conv=fdatasync
	cp pattern "$mount_dir/pattern2"

	# extend while mounted
	lvextend -L+64M $vg/${tp}_tdata "$dev4"
	lvs -a $vg -o+devices

	# verify initial data
	diff pattern "$mount_dir/pattern1"
	diff pattern "$mount_dir/pattern2"
	dd of=/dev/null if="$mount_dir/rand100M" bs=1M count=100

	# add more data
	cp pattern "$mount_dir/pattern3"
	dd if=/dev/urandom of="$mount_dir/rand8M" bs=1M count=8 conv=fdatasync

	# restart the LV
	umount "$mount_dir"
	lvchange -an $vg/$lvt
	lvchange -an $vg/$tp
	lvchange -ay $vg/$lvt
	mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"

	# verify all data
	diff pattern "$mount_dir/pattern1"
	diff pattern "$mount_dir/pattern2"
	diff pattern "$mount_dir/pattern3"
	dd of=/dev/null if="$mount_dir/rand100M" bs=1M count=100
	dd of=/dev/null if="$mount_dir/rand8M" bs=1M count=8

	# extend again while inactive
	umount "$mount_dir"
	lvchange -an $vg/$lvt
	lvchange -an $vg/$tp
	lvextend -L+64M $vg/${tp}_tdata "$dev5"
	lvs -a $vg -o+devices
	lvchange -ay $vg/$lvt
	mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"

	# verify all data
	diff pattern "$mount_dir/pattern1"
	diff pattern "$mount_dir/pattern2"
	diff pattern "$mount_dir/pattern3"
	dd of=/dev/null if="$mount_dir/rand100M" bs=1M count=100
	dd of=/dev/null if="$mount_dir/rand8M" bs=1M count=8

	# add more data
	cp pattern "$mount_dir/pattern4"

	# remove the cache
	lvconvert --splitcache $vg/${tp}_tdata

	# verify all data
	diff pattern "$mount_dir/pattern1"
	diff pattern "$mount_dir/pattern2"
	diff pattern "$mount_dir/pattern3"
	diff pattern "$mount_dir/pattern4"
	dd of=/dev/null if="$mount_dir/rand100M" bs=1M count=100
	dd of=/dev/null if="$mount_dir/rand8M" bs=1M count=8

	umount "$mount_dir"
	lvchange -an $vg/$lvt
	lvchange -an $vg/$tp
	lvchange -ay $vg/$lvt
	mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"

	# verify all data
	diff pattern "$mount_dir/pattern1"
	diff pattern "$mount_dir/pattern2"
	diff pattern "$mount_dir/pattern3"
	diff pattern "$mount_dir/pattern4"
	dd of=/dev/null if="$mount_dir/rand100M" bs=1M count=100
	dd of=/dev/null if="$mount_dir/rand8M" bs=1M count=8

	umount "$mount_dir"
	lvchange -an $vg/$lvt
	lvchange -an $vg/$tp
	lvremove $vg/$lvt
	lvremove $vg/$tp
	lvremove -y $vg
}


aux have_cache 1 10 0 || skip
aux have_writecache 1 0 0 || skip
which mkfs.xfs || skip

mount_dir="mnt"
mkdir -p "$mount_dir"

aux prepare_devs 6 70 # want 64M of usable space from each dev

# Tests with fs block sizes require a libblkid version that shows BLOCK_SIZE
vgcreate $vg "$dev1"
lvcreate -n $lv1 -L50 $vg
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE || skip
lvchange -an $vg
vgremove -ff $vg

# generate random data
dd if=/dev/urandom of=pattern bs=512K count=1

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"

# test extending cache|writecache on thin pool data
# test type cache|writecache
# cache with cachepool|cachevol
# cache with writeback|writethrough

# lv1 is thinpool LV: 128M
# lv2 is fast LV:      64M
# lv3 is thin LV:       1G

# attach writecache to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -n $lv2 -L64M -an $vg "$dev3"
lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1
lvs -a $vg -o+devices
do_test $lv1 $lv3

# attach cache/writeback (cachevol) to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -n $lv2 -L64M -an $vg "$dev3"
lvconvert -y --type cache --cachevol $lv2 --cachemode writeback $vg/$lv1
lvs -a $vg -o+devices
do_test $lv1 $lv3

# attach cache/writethrough (cachevol) to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -n $lv2 -L64M -an $vg "$dev3"
lvconvert -y --type cache --cachevol $lv2 --cachemode writethrough $vg/$lv1
lvs -a $vg -o+devices
do_test $lv1 $lv3

# attach cache (cachepool) to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -y --type cache-pool -n $lv2 -L64M --poolmetadataspare n $vg "$dev3" "$dev6"
lvconvert -y --type cache --cachepool $lv2 --poolmetadataspare n $vg/$lv1
lvs -a $vg -o+devices
do_test $lv1 $lv3

# FIXME: test these thin pool data extensions done by dmeventd

vgremove -f $vg

