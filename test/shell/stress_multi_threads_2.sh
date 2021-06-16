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

aux prepare_devs 8
get_devs

pvcreate -M2 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"

test_vg_thread1()
{
	for i in {1..1000}
	do
		vgcreate $SHARED -M2 "$vg1" "$dev1" "$dev2" "$dev3"
		vgremove -ff $vg1
	done
}

test_vg_thread2()
{
	vgcreate $SHARED -M2 "$vg2" "$dev4" "$dev5" "$dev6"

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

	vgremove -ff $vg2
}

test_vg_thread3()
{
	for i in {1..1000}
	do
		pvcreate -M2 "$dev7" "$dev8"
		pvremove "$dev7"
		pvremove "$dev8"
	done
}

test_vg_thread1 &
WAITPID=$!

test_vg_thread2 &
WAITPID="$WAITPID "$!

test_vg_thread3 &
WAITPID="$WAITPID "$!

wait $WAITPID
