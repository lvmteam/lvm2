#!/usr/bin/env bash

# Copyright (C) 2020 Seagate, Inc. All rights reserved.
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

[ -z "$LVM_TEST_LOCK_TYPE_IDM" ] && skip
[ -z "$LVM_TEST_FAILURE" ] && skip

aux prepare_devs 3
aux extend_filter_LVMTEST

DRIVE1=`dmsetup deps -o devname $dev1 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`
DRIVE2=`dmsetup deps -o devname $dev2 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`
DRIVE3=`dmsetup deps -o devname $dev3 | awk '{gsub(/[()]/,""); print $4;}' | sed 's/[0-9]*$//'`

if [ "$DRIVE1" = "$DRIVE2" ] || [ "$DRIVE1" = "$DRIVE3" ] || [ "$DRIVE2" = "$DRIVE3" ]; then
	die "Need to pass three different drives!?"
fi

# The previous device-mapper are removed, but LVM still can directly
# access VGs from the specified physical drives.  So enable drives
# for these drives.
aux extend_filter_LVMTEST "a|/dev/$DRIVE1*|" "a|/dev/$DRIVE2*|" "a|/dev/$DRIVE3*|"
aux lvmconf "devices/allow_changes_with_duplicate_pvs = 1"

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

# Create new logic volume and deactivate it
lvcreate -a y --zero n -l 1 -n $lv1 $vg

# Inject failure 40% so cannot send partially request to drives
idm_inject_failure 40

# Wait for 40s, but the lock will not be time out
sleep 40

# Inject failure with 0% so can access drives
idm_inject_failure 0

# Deactivate logic volume due to locking failure
lvchange $vg/$lv1 -a n

# Inject failure 100% so cannot send request to drives
idm_inject_failure 100

# Wait for 70s but should have no any alive locks
sleep 70

# Inject failure with 0% so can access drives
idm_inject_failure 0

# Activate logic volume
lvchange $vg/$lv1 -a y

# Inject failure so cannot send request to drives
idm_inject_failure 100

# Wait for 70s but will not time out
sleep 70

# Inject failure with 0% so can access drives
idm_inject_failure 0

check grep_lvmlockd_dump "S lvm_$vg kill_vg"
lvmlockctl --drop $vg

vgchange --lock-start
vgremove -f $vg
