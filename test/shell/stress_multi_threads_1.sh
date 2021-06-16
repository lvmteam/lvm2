#!/usr/bin/env bash

# Copyright (C) 2021 Seagate, Inc. All rights reserved.
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

[ -z "$LVM_TEST_LOCK_TYPE_IDM" ] && skip;

aux prepare_devs 6
get_devs

pvcreate -M2 "${DEVICES[@]}"

vgcreate $SHARED -M2 "$vg1" "$dev1" "$dev2" "$dev3"
vgcreate $SHARED -M2 "$vg2" "$dev4" "$dev5" "$dev6"

test_vg_thread1()
{
	for i in {1..1000}
	do
		# Create new logic volume and deactivate it
		lvcreate -a n --zero n -l 1 -n foo $vg1

		# Set minor number
		lvchange $vg1/foo -My --major=255 --minor=123

		# Activate logic volume
		lvchange $vg1/foo -a y

		# Extend logic volume with 10%
		lvextend -l+10 $vg1/foo

		# Deactivate logic volume
		lvchange $vg1/foo -a n

		# Deactivate volume group
		vgchange $vg1 -a n

		# Activate volume group with shareable mode
		vgchange $vg1 -a sy

		# lvextend fails due to mismatched lock mode
		not lvextend -l+10 $vg1/foo

		# Promote volume group to exclusive mode
		vgchange $vg1 -a ey

		lvreduce -f -l-4 $vg1/foo

		lvchange -an $vg1/foo
		lvremove $vg1/foo
	done
}

test_vg_thread2()
{
	for i in {1..1000}
	do
		# Create new logic volume and deactivate it
		lvcreate -a n --zero n -l 1 -n foo $vg2

		# Set minor number
		lvchange $vg2/foo -My --major=255 --minor=124

		# Activate logic volume
		lvchange $vg2/foo -a y

		# Extend logic volume with 10%
		lvextend -l+10 $vg2/foo

		# Deactivate logic volume
		lvchange $vg2/foo -a n

		# Deactivate volume group
		vgchange $vg2 -a n

		# Activate volume group with shareable mode
		vgchange $vg2 -a sy

		# lvextend fails due to mismatched lock mode
		not lvextend -l+10 $vg2/foo

		# Promote volume group to exclusive mode
		vgchange $vg2 -a ey

		lvreduce -f -l-4 $vg2/foo

		lvchange -an $vg2/foo
		lvremove $vg2/foo
	done
}

test_vg_thread1 &
WAITPID=$!

test_vg_thread2 &
WAITPID="$WAITPID "$!

wait $WAITPID

vgremove -ff $vg1
vgremove -ff $vg2
