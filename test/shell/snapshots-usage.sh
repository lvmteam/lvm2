#!/bin/bash
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# no automatic extensions please

. lib/test

fill() {
	dd if=/dev/zero of=$DM_DEV_DIR/$vg1/lvol0 bs=$1 count=1
}

aux prepare_vg

lvcreate -s -l 100%FREE -n $lv $vg --virtualsize 15P

aux extend_filter_LVMTEST
aux lvmconf "activation/snapshot_autoextend_percent = 20" \
            "activation/snapshot_autoextend_threshold = 50"

# Check usability with smallest extent size
pvcreate --setphysicalvolumesize 4T $DM_DEV_DIR/$vg/$lv
vgcreate -s 1K $vg1 $DM_DEV_DIR/$vg/$lv

# Check border size
lvcreate -aey -L4095G $vg1
lvcreate -s -L100K $vg1/lvol0
fill 1K
check lv_field $vg1/lvol1 data_percent "12.00"
lvremove -ff $vg1

# Create 1KB snapshot
lvcreate -aey -l1 $vg1
not lvcreate -s -l1 $vg1/lvol0
not lvcreate -s -l3 $vg1/lvol0
lvcreate -s -l30 $vg1/lvol0
check lv_field $vg1/lvol1 size "12.00k"

not lvcreate -s -c512 -l512 $vg1/lvol0
lvcreate -aey -s -c128 -l1700 $vg1/lvol0
# 3 * 128
check lv_field $vg1/lvol2 size "384.00k"

lvremove -ff $vg1

lvcreate -aey -l20 $vg1
lvcreate -s -l12 $vg1/lvol0

# Fill 1KB -> 100% snapshot (1x 4KB chunk)
fill 1K
check lv_field $vg1/lvol1 data_percent "100.00"

# Check it resizes 100% full valid snapshot
lvextend --use-policies $vg1/lvol1
check lv_field $vg1/lvol1 data_percent "80.00"

fill 4K
lvextend --use-policies $vg1/lvol1
check lv_field $vg1/lvol1 size "18.00k"

lvextend -l+33 $vg1/lvol1
check lv_field $vg1/lvol1 size "28.00k"

fill 20K
vgremove -ff $vg1

# Check usability with large extent size
pvcreate $DM_DEV_DIR/$vg/$lv
vgcreate -s 4G $vg1 $DM_DEV_DIR/$vg/$lv

lvcreate -aey -l1 $vg1
lvcreate -s -l1 $vg1/lvol0
check lv_field $vg1/lvol1 size "4.00g"

lvcreate -aey -V15E -l1 -s $vg1
check lv_field $vg1/lvol2 origin_size "15.00e"

vgremove -ff $vg1
vgremove -ff $vg
