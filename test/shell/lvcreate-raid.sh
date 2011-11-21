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
# Create, wait for sync, remove tests
###########################################

# Create RAID1 (implicit 2-way)
lvcreate --type raid1 -l 2 -n $lv1 $vg
wait_for_raid_sync $vg/$lv1
lvremove -ff $vg

# Create RAID1 (explicit 2-way)
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
wait_for_raid_sync $vg/$lv1
lvremove -ff $vg

# Create RAID1 (explicit 3-way)
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
wait_for_raid_sync $vg/$lv1
lvremove -ff $vg

# Create RAID 4/5/6 (explicit 3-stripe + parity devs)
for i in raid4 \
	raid5 raid5_ls raid5_la raid5_rs raid5_ra \
	raid6 raid6_zr raid6_nr raid6_nc; do

	lvcreate --type $i -l 3 -i 3 -n $lv1 $vg
	wait_for_raid_sync $vg/$lv1
	lvremove -ff $vg
done

#
# FIXME: Add tests that specify particular PVs to use for creation
#
