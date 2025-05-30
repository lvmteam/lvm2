#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Exercise btrfs resize'

. lib/inittest

aux have_fsinfo || skip "Test needs --fs checksize support"

aux prepare_vg 1 1024

which mkfs.btrfs || skip
which btrfs	 || skip
grep btrfs /proc/filesystems || skip

vg_lv=$vg/$lv1
vg_lv2=$vg/${lv1}bar
vg_lv3=$vg/${lv1}bar2
dev_vg_lv="$DM_DEV_DIR/$vg_lv"
dev_vg_lv2="$DM_DEV_DIR/$vg_lv2"
dev_vg_lv3="$DM_DEV_DIR/$vg_lv3"
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
	lvreduce -L64M -nf $vg_lv || true
	lvreduce -L64M -nf $vg_lv2 || true
	lvreduce -L64M -nf $vg_lv3 || true
}

fscheck_btrfs() {
	btrfsck "$1"
}
scrub_btrfs() {
	btrfs scrub start -B "$1"
}

btrfs_devid() {
	local devpath="$1"
	local devid

	devpath="$(readlink "$devpath")"

	# It could be a multi-devices btrfs so call grep.
	devid="$(LC_ALL=C btrfs filesystem show "$devpath" | grep "$devpath"$ || true)"

	# if DM_DEV_DIR is not /dev/ e.g /tmp, output of btrfs filesystem show would be like:
	# Label: none  uuid: d17f6974-267f-4140-8d71-83d4afd36a72
	# 		 Total devices 1 FS bytes used 144.00KiB
	# 		 devid    1 size 256.00MiB used 16.75MiB path /dev/mapper/LVMTEST120665vg-LV1
	#
	# But the VOLUME here is /tmp/mapper/LVMTEST120665vg-LV1
	if [ -z "$devid" ];then
		tmp_path="${devpath/#${DM_DEV_DIR}//dev}"
		devid="$(LC_ALL=C btrfs filesystem show "$devpath" | grep "$tmp_path"$)"
		devid=${devid##*devid}
	fi

	devid=${devid##*devid}
	devid=${devid%%size*}
	devid="$(echo "$devid" |sed 's/^[ \t]*//g'|sed 's/[ \t]*$'//g)"

	echo "$devid"
}

# in MiB
btrfs_device_size() {
	local dev="$1"
	local mnt="$2"
	local devid

	devid="$(btrfs_devid $dev)"
	btrfs device usage -b "$mnt" | grep -A1 "ID: $devid"$ | grep 'Device size:' \
		| awk '{print $NF}'
}

verify_mounted_device_size() {
	local dev="$1"
	local mnt="$2"
	local size="$(echo $3 | numfmt --from=iec)"
	local used_size

	used_size=$(btrfs_device_size "$dev" "$mnt")

	[[ "$used_size" == "$size"  ]]
}

verify_device_size() {
	local dev="$1"
	local size="$2"
	local mnt="$mount_dir"

	mount "$dev" "$mnt"
	verify_mounted_device_size "$dev" "$mnt" "$size"
	umount "$mnt"
}

# btrfs minimal size calculation is complex, we use 64M here.
lvcreate -n $lv1 -L64M $vg
lvcreate -n ${lv1}bar -L64M $vg
lvcreate -n ${lv1}bar2 -L64M $vg
trap 'cleanup_mounted_and_teardown' EXIT

single_device_test() {
	mkfs.btrfs -m single "$dev_vg_lv"
	mkfs.btrfs -m single "$dev_vg_lv2"


	# kernel limits 256 MB as minimal btrfs resizable size
	# you can grow fs from 64MB->256MB
	# but you can't grow from 64MB->180MB
	lvresize -y --fs resize $vg_lv -L 256M
	verify_device_size $dev_vg_lv 256M
	lvresize -y --fs resize $vg_lv2 -L 256M
	verify_device_size $dev_vg_lv2 256M

	not lvresize -y --fs resize $vg_lv -L 200M
	lvresize -y -L+12M --fs resize $vg_lv
	verify_device_size $dev_vg_lv 268M
	# lvreduce default behavior is fs checksize, must fail
	not lvreduce -y -L256M $vg_lv
	lvreduce -y -L256M --fs resize $vg_lv
	fscheck_btrfs $dev_vg_lv

	# do test on mounted state
	mount "$dev_vg_lv" "$mount_dir"
	mount "$dev_vg_lv2" "$mount_space_dir"

	# 'not': we expect a failure here.
	not lvresize --fs resize $vg_lv -L 200M
	lvresize -L+12M --fs resize $vg_lv

	verify_mounted_device_size $dev_vg_lv "$mount_dir" 268M
	# lvreduce default behavior is fs checksize, must fail
	not lvreduce -L256M $vg_lv
	lvreduce -L256M --fs resize $vg_lv
	verify_mounted_device_size $dev_vg_lv "$mount_dir" 256M
	scrub_btrfs $dev_vg_lv
	umount "$mount_dir"

	not lvresize --fs resize $vg_lv2 -L 200M
	lvresize -L+12M --fs resize $vg_lv2
	verify_mounted_device_size $dev_vg_lv2 "$mount_space_dir" 268M
	not lvreduce -L256M --fs checksize $vg_lv2
	lvreduce -L256M --fs resize $vg_lv2
	verify_mounted_device_size $dev_vg_lv2 "$mount_space_dir" 256M
	scrub_btrfs $dev_vg_lv2
	umount "$mount_space_dir"
}

multiple_devices_test() {

	# fs size is the sum of the three LVs size
	mkfs.btrfs -m single -d single -f "$dev_vg_lv" "$dev_vg_lv2" "$dev_vg_lv3"

	# The VG is 1GB size, we expect success here.
	# lv,lv2,lv3 size are changed from 64M to 256M
	lvresize --fs resize $vg_lv -L 256M --fsmode manage
	lvresize --fs resize $vg_lv2 -L 256M --fsmode manage
	lvresize --fs resize $vg_lv3 -L 256M --fsmode manage

	verify_device_size $dev_vg_lv 256M
	verify_device_size $dev_vg_lv2 256M
	verify_device_size $dev_vg_lv3 256M

	# check if lvextend/lvreduce is able to get/resize btrfs on
	# the right device
	lvextend -L+150M --fs resize $vg_lv --fsmode manage
	lvreduce --fs resize $vg_lv -L 300M --fsmode manage
	verify_device_size $dev_vg_lv 300M
	not lvreduce -y -L256M --fs checksize $vg_lv --fsmode manage
	lvreduce -L256M --fs resize $vg_lv --fsmode manage
	verify_device_size $dev_vg_lv 256M
	fscheck_btrfs $dev_vg_lv

	lvextend -L+150M --fs resize $vg_lv2 --fsmode manage
	lvreduce --fs resize $vg_lv2 -L 300M --fsmode manage
	verify_device_size $dev_vg_lv2 300M
	not lvreduce -y -L256M --fs checksize $vg_lv2 --fsmode manage
	lvreduce -y -L256M --fs resize $vg_lv2 --fsmode manage
	verify_device_size $dev_vg_lv2 256M
	fscheck_btrfs $dev_vg_lv2

	lvextend -L+150M --fs resize $vg_lv3 --fsmode manage
	lvreduce --fs resize $vg_lv3 -L 300M --fsmode manage
	verify_device_size $dev_vg_lv3 300M
	not lvreduce -y -L256M --fs checksize $vg_lv3 --fsmode manage
	lvreduce -y -L256M --fs resize $vg_lv3 --fsmode manage
	verify_device_size $dev_vg_lv3 256M
	fscheck_btrfs $dev_vg_lv3

	# repeat with mounted fs
	mount "$dev_vg_lv" "$mount_dir"

	lvresize -L300M --fs resize $vg_lv
	verify_mounted_device_size $dev_vg_lv "$mount_dir" 300M
	lvreduce -y -L256M --fs resize $vg_lv
	verify_mounted_device_size $dev_vg_lv "$mount_dir" 256M

	lvresize -L300M --fs resize $vg_lv2
	verify_mounted_device_size $dev_vg_lv2 "$mount_dir" 300M
	lvreduce -y -L256M --fs resize $vg_lv2
	verify_mounted_device_size $dev_vg_lv2 "$mount_dir" 256M

	lvresize -L300M --fs resize $vg_lv3
	verify_mounted_device_size $dev_vg_lv3 "$mount_dir" 300M
	lvreduce -y -L256M --fs resize $vg_lv3
	verify_mounted_device_size $dev_vg_lv3 "$mount_dir" 256M

	scrub_btrfs $dev_vg_lv
	umount "$mount_dir"

	lvresize -nf -L300M $vg_lv
	lvresize -nf -L300M $vg_lv2
}

single_device_test
# after each test, reset_lv_size should be called to make sure
# all lvs are in same state/size.
reset_lvs
multiple_devices_test


vgremove -ff $vg
