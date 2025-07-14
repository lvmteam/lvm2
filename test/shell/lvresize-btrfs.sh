#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Exercise BTRFS resize operations'

. lib/inittest

aux have_fsinfo || skip "Test requires --fs checksize support"

aux prepare_vg 1 1024

which mkfs.btrfs || skip
which btrfs	 || skip
grep btrfs /proc/filesystems || skip

dev_vg_lv1="$DM_DEV_DIR/$vg/$lv1"
dev_vg_lv2="$DM_DEV_DIR/$vg/$lv2"
dev_vg_lv3="$DM_DEV_DIR/$vg/$lv3"
mount_dir="mnt"
mount_space_dir="mnt space dir"

test ! -d "$mount_dir" && mkdir "$mount_dir"
test ! -d "$mount_space_dir" && mkdir "$mount_space_dir"

cleanup_mounted_and_teardown()
{
	umount "$mount_dir" || true
	umount "$mount_space_dir" || true
	aux teardown
}

reset_lvs()
{
	# Since we call mkfs.btrfs with '-f', lvreduce to 64M is enough
	lvreduce -L64M -nf $vg/$lv1 || true
	lvreduce -L64M -nf $vg/$lv2 || true
	lvreduce -L64M -nf $vg/$lv3 || true
}

fscheck_btrfs() {
	btrfsck "$1"
}
scrub_btrfs() {
	btrfs scrub start -B "$1"
}

btrfs_path_major_minor()
{
	local STAT

	STAT=$(stat --format "echo \$((0x%t)):\$((0x%T))" "$(readlink -e "$1")") || \
		die "Cannot get major:minor for \"$1\"."
	eval "$STAT"
}

btrfs_devid() {
	local devpath=$1
	local devid devinfo major_minor path_major_minor
	local IFS=$'\n'

	major_minor=$(btrfs_path_major_minor "$devpath")

	# It could be a multi-devices btrfs, filter the output.
	# Device in `btrfs filesystem show $devpath` could be /dev/mapper/* so call `readlink -e`
	for devinfo in $(LC_ALL=C btrfs filesystem show "$devpath"); do
		case "$devinfo" in
		*devid*)
			path_major_minor=$(btrfs_path_major_minor "${devinfo#* path }")
			# compare Major:Minor
			[ "$major_minor" = "$path_major_minor" ] || continue
			devid=${devinfo##*devid}
			devid=${devid%%size*}

			# trim all prefix and postfix spaces from devid
			devid=${devid#"${devid%%[![:space:]]*}"}
			echo "${devid%"${devid##*[![:space:]]}"}"
			return 0
			;;
		esac
	done

	# fail, devid not found
	return 1
}

# in MiB
btrfs_device_size() {
	local dev=$1 mnt=$2
	local devid

	devid=$(btrfs_devid $dev)
	btrfs device usage -b "$mnt" | grep -A1 "ID: $devid"$ | grep 'Device size:' \
		| awk '{print $NF}'
}

verify_mounted_device_size() {
	local dev=$1 mnt=$2
	local size used_size

	size=$(echo "$3" | numfmt --from=iec)
	used_size=$(btrfs_device_size "$dev" "$mnt")

	test "$used_size" = "$size"
}

verify_device_size() {
	local dev=$1 size=$2 mnt=$mount_dir

	mount "$dev" "$mnt"
	verify_mounted_device_size "$dev" "$mnt" "$size"
	umount "$mnt"
}

# BTRFS minimal size calculation is complex, we use 64M here.
lvcreate -n $lv1 -L64M $vg
lvcreate -n $lv2 -L64M $vg
lvcreate -n $lv3 -L64M $vg
trap 'cleanup_mounted_and_teardown' EXIT

single_device_test() {
	mkfs.btrfs -m single "$dev_vg_lv1"
	mkfs.btrfs -m single "$dev_vg_lv2"


	# The kernel limits the minimum BTRFS resizable size to 256 MB.
	# You can grow fs from 64 MB to 256 MB,
	# but you cannot grow from 64 MB to 180 MB.
	lvresize -y --fs resize $vg/$lv1 -L 256M
	verify_device_size "$dev_vg_lv1" 256M
	lvresize -y --fs resize $vg/$lv2 -L 256M
	verify_device_size "$dev_vg_lv2" 256M

	not lvresize -y --fs resize $vg/$lv1 -L 200M
	lvresize -y -L+12M --fs resize $vg/$lv1
	verify_device_size "$dev_vg_lv1" 268M
	# lvreduce default behavior is fs checksize, must fail
	not lvreduce -y -L256M $vg/$lv1
	lvreduce -y -L256M --fs resize $vg/$lv1
	fscheck_btrfs "$dev_vg_lv1"

	# do test on mounted state
	mount "$dev_vg_lv1" "$mount_dir"
	mount "$dev_vg_lv2" "$mount_space_dir"

	# 'not': we expect a failure here.
	not lvresize --fs resize $vg/$lv1 -L 200M
	lvresize -L+12M --fs resize $vg/$lv1

	verify_mounted_device_size "$dev_vg_lv1" "$mount_dir" 268M
	# lvreduce default behavior is fs checksize, must fail
	not lvreduce -L256M $vg/$lv1
	lvreduce -L256M --fs resize $vg/$lv1
	verify_mounted_device_size "$dev_vg_lv1" "$mount_dir" 256M
	scrub_btrfs "$dev_vg_lv1"
	umount "$mount_dir"

	not lvresize --fs resize $vg/$lv2 -L 200M
	lvresize -L+12M --fs resize $vg/$lv2
	verify_mounted_device_size "$dev_vg_lv2" "$mount_space_dir" 268M
	not lvreduce -L256M --fs checksize $vg/$lv2
	lvreduce -L256M --fs resize $vg/$lv2
	verify_mounted_device_size "$dev_vg_lv2" "$mount_space_dir" 256M
	scrub_btrfs "$dev_vg_lv2"
	umount "$mount_space_dir"
}

multiple_devices_test() {

	# fs size is the sum of the three LVs size
	mkfs.btrfs -m single -d single -f "$dev_vg_lv1" "$dev_vg_lv2" "$dev_vg_lv3"

	# The VG is 1GB size, we expect success here.
	# lv,lv2,lv3 size are changed from 64M to 256M
	lvresize --fs resize $vg/$lv1 -L 256M --fsmode manage
	lvresize --fs resize $vg/$lv2 -L 256M --fsmode manage
	lvresize --fs resize $vg/$lv3 -L 256M --fsmode manage

	verify_device_size "$dev_vg_lv1" 256M
	verify_device_size "$dev_vg_lv2" 256M
	verify_device_size "$dev_vg_lv3" 256M

	# check if lvextend/lvreduce is able to get/resize btrfs on
	# the right device
	lvextend -L+150M --fs resize $vg/$lv1 --fsmode manage
	lvreduce --fs resize $vg/$lv1 -L 300M --fsmode manage
	verify_device_size "$dev_vg_lv1" 300M
	not lvreduce -y -L256M --fs checksize $vg/$lv1 --fsmode manage
	lvreduce -L256M --fs resize $vg/$lv1 --fsmode manage
	verify_device_size "$dev_vg_lv1" 256M
	fscheck_btrfs "$dev_vg_lv1"

	lvextend -L+150M --fs resize $vg/$lv2 --fsmode manage
	lvreduce --fs resize $vg/$lv2 -L 300M --fsmode manage
	verify_device_size "$dev_vg_lv2" 300M
	not lvreduce -y -L256M --fs checksize $vg/$lv2 --fsmode manage
	lvreduce -y -L256M --fs resize $vg/$lv2 --fsmode manage
	verify_device_size "$dev_vg_lv2" 256M
	fscheck_btrfs "$dev_vg_lv2"

	lvextend -L+150M --fs resize $vg/$lv3 --fsmode manage
	lvreduce --fs resize $vg/$lv3 -L 300M --fsmode manage
	verify_device_size "$dev_vg_lv3" 300M
	not lvreduce -y -L256M --fs checksize $vg/$lv3 --fsmode manage
	lvreduce -y -L256M --fs resize $vg/$lv3 --fsmode manage
	verify_device_size "$dev_vg_lv3" 256M
	fscheck_btrfs "$dev_vg_lv3"

	# repeat with mounted fs
	mount "$dev_vg_lv1" "$mount_dir"

	lvresize -L300M --fs resize $vg/$lv1
	verify_mounted_device_size "$dev_vg_lv1" "$mount_dir" 300M
	lvreduce -y -L256M --fs resize $vg/$lv1
	verify_mounted_device_size "$dev_vg_lv1" "$mount_dir" 256M

	lvresize -L300M --fs resize $vg/$lv2
	verify_mounted_device_size "$dev_vg_lv2" "$mount_dir" 300M
	lvreduce -y -L256M --fs resize $vg/$lv2
	verify_mounted_device_size "$dev_vg_lv2" "$mount_dir" 256M

	lvresize -L300M --fs resize $vg/$lv3
	verify_mounted_device_size "$dev_vg_lv3" "$mount_dir" 300M
	lvreduce -y -L256M --fs resize $vg/$lv3
	verify_mounted_device_size "$dev_vg_lv3" "$mount_dir" 256M

	scrub_btrfs "$dev_vg_lv1"
	umount "$mount_dir"

	lvresize -nf -L300M $vg/$lv1
	lvresize -nf -L300M $vg/$lv2
}

single_device_test
# after each test, reset_lv_size should be called to make sure
# all lvs are in same state/size.
reset_lvs
multiple_devices_test


vgremove -ff $vg
