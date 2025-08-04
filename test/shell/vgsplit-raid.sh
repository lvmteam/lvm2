#!/usr/bin/env bash

# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test vgsplit operation, including different LV types

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

COMM() {
	LAST_TEST="$*"
}

create_vg_() {
	vgcreate -s 64k "$@"
}

aux have_raid 1 3 0 || skip

aux prepare_pvs 6 10

#
# vgsplit can be done into a new or existing VG
#
for i in new existing
do
	#
	# We can have PVs or LVs on the cmdline
	#
	for j in PV LV
	do
COMM "vgsplit correctly splits RAID LV into $i VG ($j args)"
		create_vg_ $vg1 "$dev1" "$dev2" "$dev3"
		test $i = existing && create_vg_ $vg2 "$dev5"

		lvcreate -an -Zn -l 64 --type raid5 -i 2 -n $lv1 $vg1
		if [ $j = PV ]; then
		  not vgsplit $vg1 $vg2 "$dev1"
		  not vgsplit $vg1 $vg2 "$dev2"
		  not vgsplit $vg1 $vg2 "$dev1" "$dev2"
		  vgsplit $vg1 $vg2 "$dev1" "$dev2" "$dev3"
		else
		  vgsplit -n $lv1 $vg1 $vg2
		fi
		if [ $i = existing ]; then
		  check pvlv_counts $vg2 4 1 0
		else
		  check pvlv_counts $vg2 3 1 0
		fi
		vgremove -f $vg2
	done
done

# ONLY TEST WHEN INTEGRITY IS AVAILABLE!
if aux have_integrity 1 5 0; then
for i in raid1 raid4 raid5 raid6 raid10
do
COMM "vgsplit correctly splits $i LV with integrity enabled"
		create_vg_ $vg1 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
		lvcreate -an -Zn -l 1 --type $i --raidintegrity y -n $lv1 $vg1 $dev1 $dev2 $dev3 $dev4 $dev5
		fail vgsplit $vg1 $vg2 "$dev1" 2>&1 | tee err
		grep "Can't split LV LV1 between two Volume Groups" err
		vgsplit $vg1 $vg2 "$dev6"
		vgremove -f $vg1
		vgremove -f $vg2
done
fi # END OF INTEGRITY TESTS
