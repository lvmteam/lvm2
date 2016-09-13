#!/bin/sh
# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_raid 1 7 0 || skip

aux prepare_vg 6


# Delay 1st leg so that rebuilding status characters
#  can be read before resync finished too quick.
aux delay_dev "$dev1" 0 10 $(get first_extent_sector "$dev1")

# raid0/raid0_meta don't support resynchronization
for r in raid0 raid0_meta
do
	lvcreate --yes --type raid0 -i 3 -l 1 -n $lv1 $vg
	check raid_leg_status $vg $lv1 "AAA"
	lvremove --yes $vg/$lv1
done

# raid1 supports resynchronization
lvcreate --yes --type raid1 -m 2 -l 2 -n $lv1 $vg
check raid_leg_status $vg $lv1 "aaa"
aux wait_for_sync $vg $lv1
check raid_leg_status $vg $lv1 "AAA"
lvremove --yes $vg/$lv1

# raid1 supports --nosync
lvcreate --yes --type raid1 --nosync -m 2 -l 1 -n $lv1 $vg
check raid_leg_status $vg $lv1 "AAA"
lvremove --yes $vg/$lv1

for r in raid4 raid5
do 
	# raid4/5 support resynchronization
	lvcreate --yes --type $r -i 3 -l 2 -n $lv1 $vg
	check raid_leg_status $vg $lv1 "aaaa"
	aux wait_for_sync $vg $lv1
	check raid_leg_status $vg $lv1 "AAAA"

	# raid4/5 support --nosync
	lvcreate --yes --type $r --nosync -i 3 -l 1 -n $lv2 $vg
	check raid_leg_status $vg $lv2 "AAAA"
	lvremove --yes $vg
done

# raid6 supports resynchronization
lvcreate --yes --type raid6 -i 3 -l 2 -n $lv1 $vg
check raid_leg_status $vg $lv1 "aaaaa"
aux wait_for_sync $vg $lv1
check raid_leg_status $vg $lv1 "AAAAA"
lvremove --yes $vg/$lv1

# raid6 rejects --nosync; it has to initialize P- and Q-Syndromes
not lvcreate --yes --type raid6 --nosync -i 3 -l 1 -n $lv1 $vg

# raid10 supports resynchronization
lvcreate --yes --type raid10 -m 1 -i 3 -l 2 -n $lv1 $vg
check raid_leg_status $vg $lv1 "aaaaaa"
aux wait_for_sync $vg $lv1
check raid_leg_status $vg $lv1 "AAAAAA"
aux wait_for_sync $vg $lv1
lvremove --yes $vg/$lv1

# raid10 supports --nosync
lvcreate --yes --type raid10 --nosync -m 1 -i 3 -l 1 -n $lv1 $vg
check raid_leg_status $vg $lv1 "AAAAAA"
aux wait_for_sync $vg $lv1
lvremove --yes $vg/$lv1

vgremove -ff $vg
