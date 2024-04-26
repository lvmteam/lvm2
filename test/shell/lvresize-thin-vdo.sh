#!/usr/bin/env bash

# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

aux have_thin 1 10 0 || skip
aux have_vdo 6 2 0 || skip

aux prepare_vg 1 7000

lvcreate --type thin -V10G -L5G --pooldatavdo y --name $lv1 $vg/pool

check lv_field $vg/pool_tdata size "5.00g"
lvresize -L+1G $vg/pool
check lv_field $vg/pool_tdata size "6.00g"
check lv_field $vg/pool size "6.00g"

vgremove -f $vg
