#!/bin/sh
# Copyright (C) 2011-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

########################################################
# MAIN
########################################################
aux target_at_least dm-raid 1 1 0 || skip

aux prepare_pvs 5 80
vgcreate -c n -s 256k $vg $(cat DEVICES)

###########################################
# RAID1 convert tests
###########################################
for under_snap in false true; do
for i in 1 2 3 4; do
	for j in 1 2 3 4; do
		if [ $i -eq 1 ]; then
			from="linear"
		else
			from="$i-way"
		fi
		if [ $j -eq 1 ]; then
			to="linear"
		else
			to="$j-way"
		fi

		echo -n "Converting from $from to $to"
		if $under_snap; then
			echo -n " (while under a snapshot)"
		fi
		echo

		if [ $i -eq 1 ]; then
			# Shouldn't be able to create with just 1 image
			not lvcreate --type raid1 -m 0 -l 2 -n $lv1 $vg

			lvcreate -l 2 -n $lv1 $vg
		else
			lvcreate --type raid1 -m $(($i - 1)) -l 2 -n $lv1 $vg
			aux wait_for_sync $vg $lv1
		fi

		if $under_snap; then
			lvcreate -s $vg/$lv1 -n snap -l 2
		fi

		lvconvert -m $((j - 1))  $vg/$lv1

		# FIXME: ensure no residual devices

		if [ $j -eq 1 ]; then
			check linear $vg $lv1
		fi
		lvremove -ff $vg
	done
done
done

# 3-way to 2-way convert while specifying devices
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg $dev1 $dev2 $dev3
aux wait_for_sync $vg $lv1
lvconvert -m1 $vg/$lv1 $dev2
lvremove -ff $vg

#
# FIXME: Add tests that specify particular devices to be removed
#

###########################################
# RAID1 split tests
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check lv_exists $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

# 2-way to linear/linear
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check linear $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

# 3-way to linear/2-way
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
# FIXME: Can't split off a RAID1 from a RAID1 yet
# 'should' results in "warnings"
should lvconvert --splitmirrors 2 -n $lv2 $vg/$lv1
#check linear $vg $lv1
#check lv_exists $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

###########################################
# RAID1 split + trackchanges / merge
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvconvert --splitmirrors 1 --trackchanges $vg/$lv1
check lv_exists $vg $lv1
check linear $vg ${lv1}_rimage_2
lvconvert --merge $vg/${lv1}_rimage_2
# FIXME: ensure no residual devices
lvremove -ff $vg

###########################################
# Mirror to RAID1 conversion
###########################################
for i in 1 2 3 ; do
	lvcreate --type mirror -m $i -l 2 -n $lv1 $vg
	aux wait_for_sync $vg $lv1
	lvconvert --type raid1 $vg/$lv1
	lvremove -ff $vg
done
