#!/bin/bash

# Copyright (C) 2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

# is_raid_in_sync <VG/LV>
function is_raid_in_sync()
{
	local dm_name
	local a
	local b
	local idx

	dm_name=`echo $1 | sed s:-:--: | sed s:/:-:`

	if ! a=(`dmsetup status $dm_name`); then
		echo "Unable to get sync status of $1"
		exit 1
	fi
	idx=$((${#a[@]} - 1))
	b=(`echo ${a[$idx]} | sed s:/:' ':`)

	if [ ${b[0]} != ${b[1]} ]; then
		echo "$dm_name (${a[3]}) is not in-sync"
		return 1
	fi

	echo "$dm_name (${a[3]}) is in-sync"
	return 0
}

# wait_for_raid_sync <VG/LV>
function wait_for_raid_sync()
{
	local i=0

	while ! is_raid_in_sync $1; do
		sleep 2
		i=$(($i + 1))
		if [ $i -gt 500 ]; then
			echo "Sync is taking too long - assume stuck"
			exit 1
		fi
	done
}

function is_raid_available()
{
	local a

	modprobe dm-raid
	a=(`dmsetup targets | grep raid`)
	if [ -z $a ]; then
		echo "RAID target not available"
		return 1
	fi
	if [ ${a[1]} != "v1.1.0" ]; then
		echo "Bad RAID version"
		return 1
	fi

	return 0
}

########################################################
# MAIN
########################################################
is_raid_available || exit 200

aux prepare_vg 5 80

###########################################
# RAID1 convert tests
###########################################
for i in 2 3 4; do
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
		echo "Converting from $from to $to"
		lvcreate --type raid1 -m $(($i - 1)) -l 2 -n $lv1 $vg
		wait_for_raid_sync $vg/$lv1
		lvconvert -m $((j - 1))  $vg/$lv1

		# FIXME: ensure no residual devices

		if [ $j -eq 1 ]; then
			check linear $vg $lv1
		fi
		lvremove -ff $vg
	done
done

#
# FIXME: Add tests that specify particular devices to be removed
#

###########################################
# RAID1 split tests
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
wait_for_raid_sync $vg/$lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check lv_exists $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

# 2-way to linear/linear
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
wait_for_raid_sync $vg/$lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check linear $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

# 3-way to linear/2-way
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
wait_for_raid_sync $vg/$lv1

# FIXME: Can't split off a mirror from a mirror yet
#lvconvert --splitmirrors 2 -n $lv2 $vg/$lv1
#check linear $vg $lv1
#check lv_exists $vg $lv2

# FIXME: ensure no residual devices
lvremove -ff $vg

###########################################
# RAID1 split + trackchanges / merge
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
wait_for_raid_sync $vg/$lv1
lvconvert --splitmirrors 1 --trackchanges $vg/$lv1
check lv_exists $vg $lv1
check linear $vg ${lv1}_rimage_2
lvconvert --merge $vg/${lv1}_rimage_2
# FIXME: ensure no residual devices
lvremove -ff $vg
