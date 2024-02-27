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

# Test creation of stack of external thin origins


SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

which mkfs.ext4 || skip
which fsck || skip

#
# Main
#
aux have_thin 1 3 0 || skip

aux prepare_vg 2 64

# Create original thin pool with 1st. external origin LV using thin from this pool
lvcreate -T -L10M -V20M -n $lv1 $vg/pool1
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

# 2nd. layer
lvcreate -T -L10M $vg/pool2
lvchange -an $vg/$lv1
lvchange -pr $vg/$lv1
lvcreate --type thin -n $lv2 --thinpool pool2 $vg/$lv1

lvs -ao+uuid $vg
fsck -n "$DM_DEV_DIR/$vg/$lv2"

# 3rd. layer
lvcreate -T -L10M $vg/pool3
lvchange -an $vg/$lv2
lvchange -pr $vg/$lv2
lvs -a $vg
lvcreate --type thin -n $lv3 --thinpool pool3 $vg/$lv2

fsck -n "$DM_DEV_DIR/$vg/$lv3"

# 4th. layer
lvcreate -T -L10M $vg/pool4
lvchange -an $vg/$lv3
lvchange -pr $vg/$lv3
lvcreate --type thin -n $lv4 --thinpool pool4 $vg/$lv3

lvs -a $vg
fsck -n "$DM_DEV_DIR/$vg/$lv4"

# Create multiple LVs using same external origin
# using also the 'snapshot' interface
lvcreate -s -n ${lv2}a --thinpool pool2 $vg/$lv1
lvcreate -s -n ${lv2}b --thinpool pool2 $vg/$lv1
lvcreate -s -n ${lv2}c --thinpool pool2 $vg/$lv1

lvs -a $vg

lvcreate -s -n ${lv3}a --thinpool pool3 $vg/$lv2
lvcreate -s -n ${lv3}b --thinpool pool3 $vg/$lv2
lvcreate -s -n ${lv3}c --thinpool pool3 $vg/$lv2

fsck -n "$DM_DEV_DIR/$vg/${lv3}c"

check active $vg ${lv3}a
vgchange -an $vg
check inactive $vg ${lv3}a

lvs -a $vg


#check active $vg $lv3

vgremove -ff $vg
