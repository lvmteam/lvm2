#!/usr/bin/env bash

# Copyright (C) 2020 Seagate, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


SKIP_WITH_LVMPOLLD=1

. lib/inittest

[ -z "$LVM_TEST_LOCK_TYPE_IDM" ] && skip
[ -z "$LVM_TEST_FAILURE" ] && skip

aux prepare_devs 2
aux extend_filter_LVMTEST

DRIVE1=`dmsetup deps -o devname $dev1 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`
DRIVE2=`dmsetup deps -o devname $dev2 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`

[ "$(basename -- $DRIVE1)" = "$(basename -- $DRIVE2)" ] && die "Need to pass two different drives!?"

# The previous device-mapper are removed, but LVM still can directly
# access VGs from the specified physical drives.  So enable drives
# for these drives.
aux extend_filter_LVMTEST "a|/dev/$DRIVE1*|" "a|/dev/$DRIVE2*|"
aux lvmconf "devices/allow_changes_with_duplicate_pvs = 1"

vgcreate $SHARED $vg "$dev1" "$dev2"

# Create new logic volume
lvcreate -a ey --zero n -l 100%FREE -n $lv1 $vg

drive_list=($DRIVE1)

# Find all drives with the same WWN and delete them from system,
# so that we can emulate the same drive with multiple paths are
# disconnected with system.
drive_wwn=`udevadm info /dev/${DRIVE1} | awk -F= '/E: ID_WWN=/ {print $2}'`
for dev in /dev/*; do
	if [ -b "$dev" ] && [[ ! "$dev" =~ [0-9] ]]; then
		wwn=`udevadm info "${dev}" | awk -F= '/E: ID_WWN=/ {print $2}'`
		if [ "$wwn" = "$drive_wwn" ]; then
			base_name="$(basename -- ${dev})"
			drive_list+=("$base_name")
			host_list+=(`readlink /sys/block/$base_name | awk -F'/' '{print $6}'`)
		fi
	fi
done

for d in "${drive_list[@]}"; do
	[ -f "/sys/block/$d/device/delete" ] && echo 1 > "/sys/block/$d/device/delete"
done

# Fail to create new logic volume
not lvcreate -a n --zero n -l 1 -n $lv2 $vg

# Wait for lock time out caused by drive failure
sleep 70

not check grep_lvmlockd_dump "S lvm_$vg kill_vg"

# Rescan drives so can probe the deleted drives and join back them
for h in "${host_list[@]}"; do
	[ -f "/sys/class/scsi_host/${h}/scan" ] && echo "- - -" > "/sys/class/scsi_host/${h}/scan"
done

# After the drive is reconnected, $vg should be visible again.
vgchange --lock-start
lvremove -f $vg/$lv1
lvcreate -a ey --zero n -l 1 -n $lv2 $vg
vgremove -ff $vg
