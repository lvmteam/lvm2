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

aux prepare_vg 3

for i in {1..1000}
do
	# Create new logic volume and deactivate it
	lvcreate -a n --zero n -l 1 -n foo $vg

	# Set minor number
	lvchange $vg/foo -My --major=255 --minor=123

	# Activate logic volume
	lvchange $vg/foo -a y

	# Check device mapper
	dmsetup info $vg-foo | tee info
	grep -E "^Major, minor: *[0-9]+, 123" info

	# Extend logic volume with 10%
	lvextend -l+10 $vg/foo

	# Deactivate logic volume
	lvchange $vg/foo -a n

	# Deactivate volume group
	vgchange $vg -a n

	# Activate volume group with shareable mode
	vgchange $vg -a sy

	# lvextend fails due to mismatched lock mode
	not lvextend -l+10 $vg/foo

	# Promote volume group to exclusive mode
	vgchange $vg -a ey

	lvreduce -f -l-4 $vg/foo

	lvchange -an $vg/foo
	lvremove $vg/foo
done

vgremove -ff $vg
