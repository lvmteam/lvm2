#!/usr/bin/env bash

# Copyright (C) 2024-2025 Red Hat, Inc. All rights reserved.
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

aux prepare_vg 6


lvcreate --type raid5 -i 5 -L8 -n $lv1 $vg
aux wait_for_sync $vg $lv1

lvs -ao+devices $vg

# fail 2 drives out of 4
aux error_dev "$dev2"
aux error_dev "$dev3"

lvchange -an $vg

aux enable_dev "$dev2"
aux enable_dev "$dev3"

lvconvert --yes --repair $vg/$lv1 -v

lvremove -f $vg


# Raid5 transient failure check

lvcreate --type raid5 -i 3 -L8 -n $lv1 $vg
aux wait_for_sync $vg $lv1

lvs -ao+devices $vg

# fail 2 drives out of 4
aux error_dev "$dev2"
aux error_dev "$dev3"

not lvconvert --yes --repair $vg/$lv1

# deactivate immediately
lvchange -an $vg

# Raid5 cannot activate with only 2 disks
not lvchange -ay $vg

# also it cannot be repaired
not lvconvert --yes --repair $vg/$lv1

# restore 1st. failed drive
aux enable_dev "$dev2"

# Raid5 should be now repairable
lvconvert --yes --repair $vg/$lv1

# Raid5 volume is working now
lvchange -ay $vg

# again deactivate
lvchange -an $vg

# restore 2nd. missing drive
aux enable_dev "$dev3"

# still repairable
lvconvert --yes --repair $vg/$lv1

lvchange -ay $vg

vgremove -ff $vg
