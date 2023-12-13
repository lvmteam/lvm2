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

# testing the create of thin-pool with data volume using vdo volume

SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest


#
# Main
#
aux have_thin 1 0 0 || skip
aux have_vdo 6 2 0 || skip
which mkfs.ext4 || skip

aux prepare_pvs 2 6400
get_devs

vgcreate $SHARED -s 64K "$vg" "${DEVICES[@]}"


# convert to thin-pool with VDO backend from existing VDO VG/LV
lvcreate --type thin-pool -L5G --pooldatavdo y --name $lv1 $vg

check lv_field $vg/$lv1 segtype thin-pool
check lv_field $vg/${lv1}_tdata segtype vdo -a

lvremove -f $vg


# cannot create thin as thin-pool tupe
invalid lvcreate --type-pool thin -L5G --pooldatavdo y -V20 $vg/pool


# try to create VDO _tdata without deduplication
lvcreate --type thin -L5G --pooldatavdo y --deduplication n -V20 $vg/pool

check lv_field $vg/pool_tdata vdo_deduplication "" -a

vgremove -ff $vg
