#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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

aux have_raid 1 3 0 || skip
aux raid456_replace_works || skip

aux lvmconf 'allocation/maximise_cling = 0' \
	    'allocation/mirror_logs_require_separate_pvs = 1'

aux prepare_vg 8

# It's possible small raid arrays do have problems with reporting in-sync.
# So try bigger size
RAID_SIZE=64

# RAID5 single replace
lvcreate --type raid5 -i 2 -L $RAID_SIZE -n $lv1 $vg "$dev1" "$dev2" "$dev3"
aux wait_for_sync $vg $lv1
aux disable_dev "$dev3"
lvconvert -y --repair $vg/$lv1
vgreduce --removemissing $vg
aux enable_dev "$dev3"
vgextend $vg "$dev3"
lvremove -ff $vg

# RAID6 double replace
lvcreate --type raid6 -i 3 -L $RAID_SIZE -n $lv1 $vg \
    "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
aux wait_for_sync $vg $lv1
aux disable_dev "$dev4" "$dev5"
lvconvert -y --repair $vg/$lv1
vgreduce --removemissing $vg
aux enable_dev "$dev4"
aux enable_dev "$dev5"
vgextend $vg "$dev4" "$dev5"
vgremove -ff $vg
