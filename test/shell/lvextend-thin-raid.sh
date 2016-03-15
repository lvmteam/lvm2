#!/bin/sh
# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
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

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

aux have_thin 1 0 0 || skip
aux have_raid 1 3 0 || skip

aux prepare_vg 3

lvcreate --type raid1 -l2 --nosync -n pool $vg
lvconvert --yes --thinpool $vg/pool "$dev3"

check lv_field $vg/pool seg_size_pe "2"
check lv_field $vg/pool_tdata seg_size_pe "2" -a

lvextend -l+3 $vg/pool

check lv_field $vg/pool seg_size_pe "5"
check lv_field $vg/pool_tdata seg_size_pe "5" -a

vgremove -ff $vg
