#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description="ensure pvmove works after restart of raid segment types"

. lib/inittest --skip-with-lvmlockd

aux have_raid 1 3 5 || skip

aux prepare_vg 5 20

#
# Prepare 2 raid5 devices
#
lvcreate -aey -l 20 --type raid5 -i 2 -n $lv1 $vg \
                "$dev1" "$dev2" "$dev3"

lvcreate -aey -l 20 --type raid5 -i 2 -n $lv2 $vg \
                "$dev1" "$dev2" "$dev3"

lvchange -an $vg

aux delay_dev "$dev4" 0 30 "$(get first_extent_sector "$dev4"):"

#
# Start pvmove operation with slowed down device, so we can
# catch and kill this pvmove operation and leave it 'unfinished'.
#
pvmove "$dev1" "$dev4" &
PVMOVE=$!
aux wait_pvmove_lv_ready "$vg-pvmove0"
kill $PVMOVE

# Deactivate devices after kill of pvmove operation
vgchange -an $vg || true

dmsetup table

#
# Now simulate situation after reboot, when LVs are reactivated
# and make sure restarted pvmove is not breaking them.
#
vgchange -ay $vg

# Leave pvmove to finish fast.
aux enable_dev "$dev4"
sleep 5

check lv_attr_bit health $vg/${lv1}_rimage_0 "-"
check lv_attr_bit health $vg/${lv1}_rimage_1 "-"
check lv_attr_bit health $vg/${lv2}_rimage_0 "-"
check lv_attr_bit health $vg/${lv2}_rimage_1 "-"
check lv_field $vg/$lv1 lv_health_status ""
check lv_field $vg/$lv1 lv_health_status ""

vgremove -ff $vg
