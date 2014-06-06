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

. lib/inittest

lv_devices() {
	test $3 -eq $(get lv_devices $1/$2 | wc -w)
}

########################################################
# MAIN
########################################################
aux have_raid 1 3 0 || skip

aux prepare_pvs 6 20  # 6 devices for RAID10 (2-mirror,3-stripe) test
vgcreate -s 512k $vg $(cat DEVICES)

###########################################
# Create, wait for sync, remove tests
###########################################
# Create RAID1 (implicit 2-way)
lvcreate --type raid1 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvremove -ff $vg

# Create RAID1 (explicit 2-way)
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvremove -ff $vg

# Create RAID1 (explicit 3-way)
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvremove -ff $vg

# Create RAID1 (explicit 3-way) - Set min/max recovery rate
lvcreate --type raid1 -m 2 -l 2 \
	--minrecoveryrate 50 --maxrecoveryrate 1M \
	-n $lv1 $vg
check lv_field $vg/$lv1 raid_min_recovery_rate 50
check lv_field $vg/$lv1 raid_max_recovery_rate 1024
aux wait_for_sync $vg $lv1
lvremove -ff $vg

# Create RAID 4/5/6 (explicit 3-stripe + parity devs)
for i in raid4 \
	raid5 raid5_ls raid5_la raid5_rs raid5_ra \
	raid6 raid6_zr raid6_nr raid6_nc; do

	lvcreate --type $i -l 3 -i 3 -n $lv1 $vg
	aux wait_for_sync $vg $lv1
	lvremove -ff $vg
done

# Create RAID 4/5/6 (explicit 3-stripe + parity devs) - Set min/max recovery
for i in raid4 \
	raid5 raid5_ls raid5_la raid5_rs raid5_ra \
	raid6 raid6_zr raid6_nr raid6_nc; do

	lvcreate --type $i -l 3 -i 3 \
		--minrecoveryrate 50 --maxrecoveryrate 1M \
		-n $lv1 $vg
	check lv_field $vg/$lv1 raid_min_recovery_rate 50
	check lv_field $vg/$lv1 raid_max_recovery_rate 1024
	aux wait_for_sync $vg $lv1
	lvremove -ff $vg
done

# Create RAID using 100%FREE
############################
# 6 PVs with 18.5m in each PV.
# 1 metadata LV = 1 extent   = .5m
# 1 image = 36+37+37 extents = 55.00m = lv_size
lvcreate --type raid1 -m 1 -l 100%FREE -an -Zn -n raid1 $vg
check lv_field $vg/raid1 size "55.00m"
lvremove -ff $vg

# 1 metadata LV = 1 extent
# 1 image = 36 extents
# 5 images = 180 extents = 90.00m = lv_size
lvcreate --type raid5 -i 5 -l 100%FREE -an -Zn -n raid5 $vg
should check lv_field $vg/raid5 size "90.00m"
#FIXME: Currently allocates incorrectly at 87.50m
lvremove -ff $vg

# 1 image = 36+37 extents
# 2 images = 146 extents = 73.00m = lv_size
lvcreate --type raid5 -i 2 -l 100%FREE -an -Zn -n raid5 $vg
check lv_field $vg/raid5 size "73.00m"
lvremove -ff $vg

# 1 image = 36 extents
# 4 images = 144 extents = 72.00m = lv_size
lvcreate --type raid6 -i 4 -l 100%FREE -an -Zn -n raid6 $vg
should check lv_field $vg/raid6 size "72.00m"
#FIXME: Currnently allocates incorrectly at 70.00m
lvremove -ff $vg

###
# For following tests eat 18 of 37 extents from dev1, leaving 19
lvcreate -l 18 -an -Zn -n eat_space $vg "$dev1"
EAT_SIZE=$(get lv_field $vg/eat_space size)

# Using 100% free should take the rest of dev1 and equal from dev2
# 1 meta takes 1 extent
# 1 image = 18 extents = 9.00m = lv_size
lvcreate --type raid1 -m 1 -l 100%FREE -an -Zn -n raid1 $vg "$dev1" "$dev2"
check lv_field $vg/raid1 size "9.00m"
# Ensure image size is the same as the RAID1 size
check lv_field $vg/raid1 size $(get lv_field $vg/raid1_rimage_0 size -a)
# Amount remaining in dev2 should equal the amount taken by 'lv' in dev1
check pv_field "$dev2" pv_free "$EAT_SIZE"
lvremove -ff $vg/raid1

# Using 100% free should take the rest of dev1 and equal amount from the rest
# 1 meta takes 1 extent
# 1 image = 18 extents = 9.00m
# 5 images = 90 extents = 45.00m = lv_size
lvcreate --type raid5 -i 5 -l 100%FREE -an -Zn -n raid5 $vg
check lv_field $vg/raid5 size "45.00m"
# Amount remaining in dev6 should equal the amount taken by 'lv' in dev1
check pv_field "$dev6" pv_free "$EAT_SIZE"
lvremove -ff $vg/raid5

# Using 100% free should take the rest of dev1, an equal amount
# from 2 more devs, and all extents from 3 additional devs
# 1 meta takes 1 extent
# 1 image = 18+37 extents
# 2 images = 110 extents = 55.00m = lv_size
lvcreate --type raid5 -i 2 -l 100%FREE -an -Zn -n raid5 $vg
check lv_field $vg/raid5 size "55.00m"
lvremove -ff $vg/raid5

# Let's do some stripe tests too
# Using 100% free should take the rest of dev1 and an equal amount from rest
# 1 image = 19 extents
# 6 images = 114 extents = 57.00m = lv_size
lvcreate -i 6 -l 100%FREE -an -Zn -n stripe $vg
check lv_field $vg/stripe size "57.00m"
lvremove -ff $vg/stripe

# Using 100% free should take the rest of dev1, an equal amount from
#  one more dev, and all of the remaining 4
# 1 image = 19+37+37 extents
# 2 images = 186 extents = 93.00m = lv_size
lvcreate -i 2 -l 100%FREE -an -Zn -n stripe $vg
check lv_field $vg/stripe size "93.00m"

lvremove -ff $vg
# end of use of '$vg/eat_space'
###

# Create RAID (implicit stripe count based on PV count)
#######################################################

# Not enough drives
not lvcreate --type raid1 -l1 $vg "$dev1"
not lvcreate --type raid5 -l2 $vg "$dev1" "$dev2"
not lvcreate --type raid6 -l3 $vg "$dev1" "$dev2" "$dev3" "$dev4"

# Implicit count comes from #PVs given (always 2 for mirror though)
lvcreate --type raid1 -l1 -an -Zn -n raid1 $vg "$dev1" "$dev2"
lv_devices $vg raid1 2
lvcreate --type raid5 -l2 -an -Zn -n raid5 $vg "$dev1" "$dev2" "$dev3"
lv_devices $vg raid5 3
lvcreate --type raid6 -l3 -an -Zn -n raid6 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lv_devices $vg raid6 5
lvremove -ff $vg

# Implicit count comes from total #PVs in VG (always 2 for mirror though)
lvcreate --type raid1 -l1 -an -Zn -n raid1 $vg
lv_devices $vg raid1 2
lvcreate --type raid5 -l2 -an -Zn -n raid5 $vg
lv_devices $vg raid5 6
lvcreate --type raid6 -l3 -an -Zn -n raid6 $vg
lv_devices $vg raid6 6

vgremove -ff $vg
