#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
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
aux target_at_least dm-raid 1 3 0 || skip

aux prepare_pvs 6 20  # 6 devices for RAID10 (2-mirror,3-stripe) test
vgcreate -s 512k $vg $(cat DEVICES)

#
# Create RAID10:
#


# Should not allow more than 2-way mirror
not lvcreate --type raid10 -m 2 -i 2 -l 2 -n $lv1 $vg

# 2-way mirror, 2-stripes
lvcreate --type raid10 -m 1 -i 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvremove -ff $vg/$lv1

# 2-way mirror, 2-stripes - Set min/max recovery rate
lvcreate --type raid10 -m 1 -i 2 -l 2 \
	--minrecoveryrate 50 --maxrecoveryrate 100 \
	-n $lv1 $vg
check lv_field $vg/$lv1 raid_min_recovery_rate 50
check lv_field $vg/$lv1 raid_max_recovery_rate 100
aux wait_for_sync $vg $lv1

# 2-way mirror, 3-stripes
lvcreate --type raid10 -m 1 -i 3 -l 3 -n $lv2 $vg
aux wait_for_sync $vg $lv2

lvremove -ff $vg

#
# FIXME: Add tests that specify particular PVs to use for creation
#
