#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/inittest --skip-with-lvmpolld

aux have_raid 1 3 0 || skip

aux prepare_vg 3

#
# Test 1: RAID1 with transient device failure - should show "refresh needed"
#

lvcreate --type raid1 -m 2 -l 1 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
aux wait_for_sync $vg $lv1

check lv_field $vg/$lv1 lv_health_status ""
check lv_attr_bit health $vg/$lv1 "-"

aux disable_dev "$dev2"
lvchange --refresh --activationmode partial $vg/$lv1
aux enable_dev "$dev2"

check lv_field $vg/$lv1 lv_health_status "refresh needed"
check lv_attr_bit health $vg/$lv1 "r"

# Also check sub-LV health status
check lv_field $vg/${lv1}_rimage_1 lv_health_status "refresh needed" -a
check lv_attr_bit health $vg/${lv1}_rimage_1 "r" -a

lvs $vg/$lv1 2>&1 | tee out
grep "needs to be refreshed" out

lvchange --refresh $vg/$lv1
aux wait_for_sync $vg $lv1

check lv_field $vg/$lv1 lv_health_status ""
check lv_attr_bit health $vg/$lv1 "-"

lvremove -ff $vg/$lv1

#
# Test 2: RAID1 with permanent device removal - should show "repair needed"
#

lvcreate --type raid1 -m 2 -l 1 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
aux wait_for_sync $vg $lv1

aux disable_dev "$dev2"
vgreduce --removemissing --force $vg

check lv_field $vg/$lv1 lv_health_status "repair needed"
check lv_attr_bit health $vg/$lv1 "r"

# Also check sub-LV health status
check lv_field $vg/${lv1}_rimage_1 lv_health_status "repair needed" -a
check lv_attr_bit health $vg/${lv1}_rimage_1 "r" -a

lvs $vg/$lv1 2>&1 | tee out
grep "needs to be repaired" out

aux enable_dev "$dev2"
vgck --updatemetadata $vg
vgextend $vg "$dev2"
lvconvert --yes --repair $vg/$lv1
aux wait_for_sync $vg $lv1

check lv_field $vg/$lv1 lv_health_status ""
check lv_attr_bit health $vg/$lv1 "-"

vgremove -ff $vg
