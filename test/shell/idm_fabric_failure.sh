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

[ -z "$LVM_TEST_FAILURE" ] && skip

aux prepare_devs 3
aux extend_filter_LVMTEST

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

# Create new logic volume
lvcreate -a ey --zero n -l 50%FREE -n $lv1 $vg

DRIVE1=`dmsetup deps -o devname $dev1 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`
DRIVE2=`dmsetup deps -o devname $dev2 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`
DRIVE3=`dmsetup deps -o devname $dev3 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`

HOST1=`readlink /sys/block/$DRIVE1 | awk -F'/' '{print $6}'`
HOST2=`readlink /sys/block/$DRIVE2 | awk -F'/' '{print $6}'`
HOST3=`readlink /sys/block/$DRIVE3 | awk -F'/' '{print $6}'`

# Emulate fabric failure
echo 1 > "/sys/block/$DRIVE1/device/delete"
[ -f "/sys/block/$DRIVE2/device/delete" ] && echo 1 > "/sys/block/$DRIVE2/device/delete"
[ -f "/sys/block/$DRIVE3/device/delete" ] && echo 1 > "/sys/block/$DRIVE3/device/delete"

# Wait for 10s and will not lead to timeout
sleep 10

# Rescan drives so can probe the deleted drives and join back them
echo "- - -" > "/sys/class/scsi_host/${HOST1}/scan"
echo "- - -" > "/sys/class/scsi_host/${HOST2}/scan"
echo "- - -" > "/sys/class/scsi_host/${HOST3}/scan"

not check grep_lvmlockd_dump "S lvm_$vg kill_vg"

# The previous device-mapper are removed, but LVM still can directly
# access VGs from the specified physical drives.  So enable drives
# for these drives.
aux extend_filter_LVMTEST "a|/dev/$DRIVE1*|" "a|/dev/$DRIVE2*|" "a|/dev/$DRIVE3*|"
aux lvmconf "devices/allow_changes_with_duplicate_pvs = 1"

lvcreate -a n --zero n -l 10 -n $lv2 $vg

vgremove -ff $vg
