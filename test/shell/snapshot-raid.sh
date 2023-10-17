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

# Test snapshots of raid

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_raid 1 3 0 || skip
which mkfs.ext4 || skip

mount_dir="mnt"
mkdir -p "$mount_dir"

snap_dir="mnt_snap"
mkdir -p "$snap_dir"

# add and remove a snapshot

test_add_del_snap() {
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
	mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

	touch "$mount_dir/B"
	not ls "$snap_dir/B"
	touch "$snap_dir/C"
	not ls "$mount_dir/C"
	ls "$mount_dir/A"
	ls "$snap_dir/A"

	umount "$snap_dir"
	lvremove -y $vg/snap
	umount "$mount_dir"
}

# add and remove snapshot while origin has a missing raid image

test_snap_with_missing_image() {
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	aux disable_dev "$dev1"
	lvs -a -o+devices $vg

	not lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"

	aux enable_dev "$dev1"
	aux wait_for_sync $vg $lv1

	lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"

	aux disable_dev "$dev1"
	lvs -a -o+devices $vg

	lvremove -y $vg/snap

	aux enable_dev "$dev1"
	vgextend --restoremissing $vg "$dev1"
	lvs -a -o+devices $vg
	aux wait_for_sync $vg $lv1

	umount "$mount_dir"
}

# raid image is lost and restored while a snapshot exists

test_missing_image_with_snap() {
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
	mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

	aux disable_dev "$dev1"
	lvs -a -o+devices $vg

	touch "$mount_dir/B"
	not ls "$snap_dir/B"
	touch "$snap_dir/C"
	not ls "$mount_dir/C"
	ls "$mount_dir/A"
	ls "$snap_dir/A"

	aux enable_dev "$dev1"
	aux wait_for_sync $vg $lv1

	ls "$mount_dir/B"
	ls "$snap_dir/C"

	umount "$snap_dir"
	lvremove -y $vg/snap
	umount "$mount_dir"
}

# add and remove raid image while snapshot exists

test_add_del_image_with_snap() {
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
	mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

	touch "$mount_dir/B"
	touch "$snap_dir/C"

	lvconvert -y -m+1 $vg/$lv1 "$dev4"
	aux wait_for_sync $vg $lv1

	ls "$mount_dir/B"
	ls "$snap_dir/C"
	ls "$mount_dir/A"
	ls "$snap_dir/A"

	touch "$mount_dir/B2"
	touch "$snap_dir/C2"

	lvconvert -y -m-1 $vg/$lv1 "$dev4"

	ls "$mount_dir/B"
	ls "$snap_dir/C"
	ls "$mount_dir/A"
	ls "$snap_dir/A"
	ls "$mount_dir/B2"
	ls "$snap_dir/C2"
	umount "$snap_dir"
	lvremove -y $vg/snap

	umount "$mount_dir"
}

test_replace_image_with_snap() {
	# add an image to replace
	lvconvert -y -m+1 $vg/$lv1 "$dev4"
	aux wait_for_sync $vg $lv1

	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
	mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

	touch "$mount_dir/B"
	touch "$snap_dir/C"

	lvconvert -y --replace "$dev4" $vg/$lv1 "$dev5"
	aux wait_for_sync $vg $lv1

	ls "$mount_dir/B"
	ls "$snap_dir/C"
	ls "$mount_dir/A"
	ls "$snap_dir/A"

	touch "$mount_dir/B2"
	touch "$snap_dir/C2"

	umount "$snap_dir"
	lvremove -y $vg/snap

	# put lv1 back to original state with images on dev1 and dev2
	lvconvert -y -m-1 $vg/$lv1 "$dev5"

	umount "$mount_dir"
}

test_repair_image_with_snap() {
	# add an image to repair
	lvconvert -y -m+1 $vg/$lv1 "$dev4"
	aux wait_for_sync $vg $lv1

	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
	mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

	touch "$mount_dir/B"
	touch "$snap_dir/C"

	aux disable_dev "$dev4"
	lvs -a -o+devices $vg

	lvconvert -y --repair $vg/$lv1 "$dev5"
	aux wait_for_sync $vg $lv1

	ls "$mount_dir/B"
	ls "$snap_dir/C"
	ls "$mount_dir/A"
	ls "$snap_dir/A"

	touch "$mount_dir/B2"
	touch "$snap_dir/C2"

	umount "$snap_dir"
	lvremove -y $vg/snap

	aux enable_dev "$dev4"
	lvs -a -o+devices $vg
	vgck --updatemetadata $vg

	# put lv1 back to original state with images on dev1 and dev2
	lvconvert -y -m-1 $vg/$lv1 "$dev5"

	umount "$mount_dir"
}

test_merge_snap()
{
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
	mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

	touch "$mount_dir/B"
	touch "$snap_dir/C"

	umount "$snap_dir"

	lvconvert --merge $vg/snap

	# the merge will begin once the origin is not in use
	umount "$mount_dir"

	lvs -a $vg
	lvchange -an $vg/$lv1
	lvchange -ay $vg/$lv1
	lvs -a $vg

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	ls "$mount_dir/A"
	ls "$mount_dir/C"
	not ls "$mount_dir/B"

	umount "$mount_dir"

	for i in $(seq 1 10); do
		# Wait tiil snapshot is surely merged
		dmsetup info $vg-snap || break
		sleep 0.1
	done
}

test_extend_snap()
{
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L8M $vg/$lv1 "$dev3"
	mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

	touch "$mount_dir/B"
	touch "$snap_dir/C"

	lvextend -L+8M $vg/snap

	umount "$mount_dir"
	umount "$snap_dir"
	lvremove -y $vg/snap
}

test_fill_snap()
{
	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
	touch "$mount_dir/A"

	lvcreate -s -n snap -L4M $vg/$lv1 "$dev3"

	lvs -a $vg
	get lv_field $vg/snap lv_attr | grep "swi-a-s---"

	dd if=/dev/zero of="$mount_dir/1" bs=1M count=1 oflag=direct
	dd if=/dev/zero of="$mount_dir/2" bs=1M count=1 oflag=direct
	dd if=/dev/zero of="$mount_dir/3" bs=1M count=1 oflag=direct
	dd if=/dev/zero of="$mount_dir/4" bs=1M count=1 oflag=direct
	dd if=/dev/zero of="$mount_dir/5" bs=1M count=1 oflag=direct

	lvs -a $vg
	get lv_field $vg/snap lv_attr | grep "swi-I-s---"
	check lv_field $vg/snap data_percent "100.00"

	umount "$mount_dir"
	lvremove -y $vg/snap
}

aux prepare_devs 5 200

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"

lvcreate --type raid1 -m1 -n $lv1 -L32M $vg "$dev1" "$dev2"
dmsetup table
aux wait_for_sync $vg $lv1
test_add_del_snap
test_snap_with_missing_image
test_missing_image_with_snap
test_add_del_image_with_snap
test_replace_image_with_snap
test_repair_image_with_snap
test_merge_snap
test_extend_snap
test_fill_snap
lvremove -y $vg/$lv1

# INTEGRITY TESTS FOLLOWING:
if aux have_integrity 1 5 0; then

lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -L32M $vg "$dev1" "$dev2"
aux wait_recalc $vg ${lv1}_rimage_0
aux wait_recalc $vg ${lv1}_rimage_1
aux wait_for_sync $vg $lv1
test_add_del_snap
test_snap_with_missing_image
test_missing_image_with_snap
test_add_del_image_with_snap
test_replace_image_with_snap
test_repair_image_with_snap
test_merge_snap
test_extend_snap
test_fill_snap
lvremove -y $vg/$lv1

# Repeat above with cache|writecache on the raid image? 

#
# Add/remove integrity while a snapshot exists
#

lvcreate --type raid1 -m1 -n $lv1 -L32M $vg "$dev1" "$dev2"
aux wait_for_sync $vg $lv1
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
touch "$mount_dir/A"

lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

touch "$mount_dir/B"
touch "$snap_dir/C"

lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg ${lv1}_rimage_0
aux wait_recalc $vg ${lv1}_rimage_1

ls "$mount_dir/B"
ls "$snap_dir/C"
ls "$mount_dir/A"
ls "$snap_dir/A"

touch "$mount_dir/B2"
touch "$snap_dir/C2"

lvconvert --raidintegrity n $vg/$lv1

ls "$mount_dir/B"
ls "$snap_dir/C"
ls "$mount_dir/A"
ls "$snap_dir/A"
ls "$mount_dir/B2"
ls "$snap_dir/C2"
umount "$snap_dir"
umount "$mount_dir"
lvremove -y $vg/snap
lvremove -y $vg/$lv1

#
# Add integrity not allowed with missing image and snapshot exists
#

lvcreate --type raid1 -m1 -n $lv1 -L32M $vg "$dev1" "$dev2"
aux wait_for_sync $vg $lv1
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
touch "$mount_dir/A"

lvcreate -s -n snap -L12M $vg/$lv1 "$dev3"
mount "$DM_DEV_DIR/$vg/snap" "$snap_dir"

touch "$mount_dir/B"
touch "$snap_dir/C"

aux disable_dev "$dev1"
lvs -a $vg

not lvconvert --raidintegrity y $vg/$lv1

aux enable_dev "$dev1"
lvs -a $vg

umount "$snap_dir"
umount "$mount_dir"
lvremove -y $vg/snap
lvremove -y $vg/$lv1

fi # INTEGRITY TESTS SKIPPED

vgremove -ff $vg
