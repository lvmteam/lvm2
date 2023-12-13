#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# testing conversion of volumes to thin-pool with VDO data LV

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

prepare_lvs() {
	lvremove -f $vg
	lvcreate -L10M -n $lv1 $vg
	lvcreate -L8M -n $lv2 $vg
}

#
# Main
#
aux have_thin 1 0 0 || skip
aux have_vdo 6 2 0 || skip

aux prepare_vg 4 6400


# convert to thin-pool with VDO backend from existing VG/LV
lvcreate -L5G --name $lv1 $vg
lvconvert --yes --type thin-pool -c 256K --pooldatavdo y $vg/$lv1

check lv_field $vg/$lv1 segtype thin-pool
check lv_field $vg/${lv1}_tdata segtype vdo -a

lvremove -f $vg


# convert to thin-pool with VDO backend from existing VDO VG/LV
lvcreate -L5G --vdo --name $lv1 $vg
lvconvert --yes --type thin-pool --pooldatavdo y $vg/$lv1

check lv_field $vg/$lv1 segtype thin-pool
check lv_field $vg/${lv1}_tdata segtype vdo -a

lvremove -f $vg

vgremove -ff $vg
