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

# Test snapshot on cache|writecache

SKIP_WITH_LVMPOLLD=1

. lib/inittest

lvm segtypes 2>/dev/null | grep writecache$ >/dev/null || {
	skip 'Writecache is not built-in.'
}
aux have_cache 1 10 0 || skip
which mkfs.ext4 || skip

HAVE_WRITECACHE=1
aux have_writecache 1 0 0 || HAVE_WRITECACHE=0

mount_dir="mnt"
mkdir -p "$mount_dir"

mount_dir_snap="mnt_snap"
mkdir -p "$mount_dir_snap"

# generate random data
dd if=/dev/urandom of=pattern1 bs=512K count=1

aux prepare_devs 2 310

vgcreate $SHARED $vg "$dev1" "$dev2"

# creating a snapshot on top of a cache|writecache

test_snap_create() {
	# cache | writecache
	local convert_type=$1

	# --cachepool | --cachevol
	local convert_option=$2

	lvcreate -n $lv1 -L 300 -an $vg "$dev1"
	lvcreate -n fast -l 4 -an $vg "$dev2"
	lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
	lvchange -ay $vg/$lv1
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	cp pattern1 "$mount_dir/pattern1a"
	lvcreate -s -L 32 -n snap $vg/$lv1
	if [ "$2" = "--cachevol" ] && [ "$1" = "cache" ]; then
		# For cachevol ensure -cmeta will have 1 line
		dm_table $vg-fast_cvol-cmeta | tee out
		test "$(wc -l < out)" = 1 || {
			die "More then 1 table line for -cmeta device"
		}
	fi
	cp pattern1 "$mount_dir/pattern1b"
	mount "$DM_DEV_DIR/$vg/snap" "$mount_dir_snap"
	not ls "$mount_dir_snap/pattern1b"
	rm "$mount_dir/pattern1a"
	diff pattern1 "$mount_dir_snap/pattern1a"
	umount "$mount_dir_snap"
	lvconvert --splitcache $vg/$lv1
	umount "$mount_dir"
	lvchange -an $vg/$lv1
	lvchange -an $vg/fast
	lvremove $vg/snap
	lvremove $vg/$lv1
	lvremove $vg/fast
}

test_snap_create cache --cachepool
test_snap_create cache --cachevol

[ "$HAVE_WRITECACHE" = "1" ] && test_snap_create writecache --cachevol

# removing cache|writecache while snapshot exists

test_snap_remove() {
	# cache | writecache
	local convert_type=$1

	# --cachepool | --cachevol
	local convert_option=$2

	lvcreate -n $lv1 -L 300 -an $vg "$dev1"
	lvcreate -n fast -l 4 -an $vg "$dev2"
	lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
	lvchange -ay $vg/$lv1
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	cp pattern1 "$mount_dir/pattern1a"
	lvcreate -s -L 32 -n snap $vg/$lv1
	cp pattern1 "$mount_dir/pattern1b"
	lvconvert --splitcache $vg/$lv1
	fsck -n "$DM_DEV_DIR/$vg/snap"
	mount "$DM_DEV_DIR/$vg/snap" "$mount_dir_snap"
	not ls "$mount_dir_snap/pattern1b"
	rm "$mount_dir/pattern1a"
	diff pattern1 "$mount_dir_snap/pattern1a"
	umount "$mount_dir_snap"
	umount "$mount_dir"
	lvchange -an $vg/$lv1
	lvchange -an $vg/fast
	lvremove $vg/snap
	lvremove $vg/$lv1
	lvremove $vg/fast
}

test_snap_remove cache --cachepool
test_snap_remove cache --cachevol

[ "$HAVE_WRITECACHE" = "1" ] && test_snap_remove writecache --cachevol

# adding cache|writecache to an LV that has a snapshot

test_caching_with_snap() {
	# cache | writecache
	local convert_type=$1

	# --cachepool | --cachevol
	local convert_option=$2

	lvcreate -n $lv1 -L 300 -an $vg "$dev1"
	lvcreate -n fast -l 4 -an $vg "$dev2"
	lvchange -ay $vg/$lv1
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	cp pattern1 "$mount_dir/pattern1a"
	lvcreate -s -L 32 -n snap $vg/$lv1
	lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
	cp pattern1 "$mount_dir/pattern1b"
	mount "$DM_DEV_DIR/$vg/snap" "$mount_dir_snap"
	not ls "$mount_dir_snap/pattern1b"
	mv "$mount_dir/pattern1a" "$mount_dir/pattern1c"
	diff pattern1 "$mount_dir_snap/pattern1a"
	lvconvert --splitcache $vg/$lv1
	diff pattern1 "$mount_dir/pattern1c"
	diff pattern1 "$mount_dir_snap/pattern1a"
	umount "$mount_dir_snap"
	umount "$mount_dir"
	lvchange -an $vg/$lv1
	lvchange -an $vg/fast
	lvremove $vg/snap
	lvremove $vg/$lv1
	lvremove $vg/fast
}

test_caching_with_snap cache --cachepool
test_caching_with_snap cache --cachevol

[ "$HAVE_WRITECACHE" = "1" ] && test_caching_with_snap writecache --cachevol

# adding cache|writecache to a snapshot is not allowed

lvcreate -n $lv1 -L 300 $vg "$dev1"
lvcreate -n fast -l 4 $vg "$dev2"
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
lvcreate -s -L 32 -n snap $vg/$lv1
not lvconvert -y --type writecache --cachevol fast $vg/snap
not lvconvert -y --type cache --cachevol fast $vg/snap
not lvconvert -y --type cache --cachepool fast $vg/snap
vgchange -an $vg
lvremove $vg/snap
lvremove $vg/$lv1
lvremove $vg/fast

vgremove -ff $vg
