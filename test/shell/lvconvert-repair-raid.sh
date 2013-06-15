#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux target_at_least dm-raid 1 1 0 || skip

aux lvmconf 'allocation/maximise_cling = 0'
aux lvmconf 'allocation/mirror_logs_require_separate_pvs = 1'

aux prepare_vg 8

# RAID5 single replace
lvcreate --type raid5 -i 2 -l 2 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
aux wait_for_sync $vg $lv1
aux disable_dev "$dev3"
lvconvert -y --repair $vg/$lv1
vgreduce --removemissing $vg
aux enable_dev "$dev3"
vgextend $vg "$dev3"
lvremove -ff $vg

# RAID6 double replace
lvcreate --type raid5 -i 3 -l 2 -n $lv1 $vg \
    "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
aux wait_for_sync $vg $lv1
aux disable_dev "$dev4" "$dev5"
lvconvert -y --repair $vg/$lv1
vgreduce --removemissing $vg
aux enable_dev "$dev4"
aux enable_dev "$dev5"
vgextend $vg "$dev4" "$dev5"
vgremove -ff $vg
