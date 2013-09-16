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
	--minrecoveryrate 50 --maxrecoveryrate 100 \
	-n $lv1 $vg
check lv_field $vg/$lv1 raid_min_recovery_rate 50
check lv_field $vg/$lv1 raid_max_recovery_rate 100
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
		--minrecoveryrate 50 --maxrecoveryrate 100 \
		-n $lv1 $vg
	check lv_field $vg/$lv1 raid_min_recovery_rate 50
	check lv_field $vg/$lv1 raid_max_recovery_rate 100
	aux wait_for_sync $vg $lv1
	lvremove -ff $vg
done
