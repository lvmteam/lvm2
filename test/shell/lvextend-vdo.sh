#!/usr/bin/env bash

# Copyright (C) 2019 Red Hat, Inc. All rights reserved.
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

aux have_vdo 6 2 0 || skip

aux lvmconf 'allocation/vdo_slab_size_mb = 128'

aux prepare_vg 1 7000
lvcreate --vdo -V3G -L4G -n $lv1 $vg/$lv2

# Resize data volume
lvextend -L+1G $vg/$lv2
check lv_field $vg/$lv2 size "5.00g"
check lv_field $vg/${lv2}_vdata size "5.00g"

# Resize virtual volume on top of VDO
lvextend -L+1G $vg/$lv1
check lv_field $vg/$lv1 size "4.00g"

vgremove -ff $vg
