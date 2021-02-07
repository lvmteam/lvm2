#!/usr/bin/env bash

# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
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

aux have_thin 1 0 0 || skip

aux prepare_vg 3
lvcreate -i2 -l2 -T $vg/pool2
lvextend -l+2 $vg/pool2 "$dev2" "$dev3"
lvextend -l+100%FREE $vg/pool2

lvremove -f $vg

lvcreate -L1 -n pool $vg
# Does work only with thin-pools
not lvextend --poolmetadatasize +1 $vg/pool
lvconvert -y --thinpool $vg/pool --poolmetadatasize 2

# _tdata cannot be used with --poolmetadata
not lvextend --poolmetadatasize +1 $vg/pool_tdata
lvextend --poolmetadatasize +1 $vg/pool_tmeta
lvextend --poolmetadatasize +1 --size +1 $vg/pool
check lv_field $vg/pool_tmeta size "4.00m"
check lv_field $vg/lvol0_pmspare size "4.00m"

not lvresize --poolmetadatasize -1 $vg/pool

vgremove -ff $vg
