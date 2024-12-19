#!/usr/bin/env bash

# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


SKIP_WITH_LVMPOLLD=1

. lib/inittest

# raid5 target version 1.7 is crashing kernel with this test
#
# BUG: unable to handle kernel NULL pointer dereference at 00000000000002f0
# raid5_free+0x15/0x30 [raid456]
#
# So possibly lvm2 needs to check for more things here.
#
aux have_raid 1 8 0 || skip

aux prepare_vg 4
get_devs

#offset=$(get first_extent_sector "$dev1")

# It's possible small raid arrays do have problems with reporting in-sync.
# So try bigger size
RAID_SIZE=8

# RAID1 transient failure check
lvcreate --type raid5 -i 3 -L $RAID_SIZE -n $lv1 $vg
aux wait_for_sync $vg $lv1

lvs -ao+devices $vg

# fail 2 drives out of 4
aux error_dev "$dev2"
aux error_dev "$dev3"

# deactivate immediately
lvchange -an $vg

aux enable_dev "$dev2"
aux enable_dev "$dev3"

# ATM we are failing here with this kernel message:
#
# md/raid:mdX: Cannot continue operation (2/4 failed).
#
# Raid5 LV cannot be started any more
should lvchange -ay $vg

vgremove -ff $vg
