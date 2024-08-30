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

# Test conversion to thin external origin stack

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

which mkfs.ext4 || skip
which fsck || skip

_prepare() {
	lvcreate -L10 -T -V20 -n $lv1 $vg/pool1

	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
	mkdir -p mnt
	mount "$DM_DEV_DIR/$vg/$lv1" mnt
	touch mnt/test1
	sync
}

#
# Main
#
aux have_thin 1 5 0 || skip

aux prepare_vg 2

_prepare

# We can build chain of external origins with the same thin-pool
lvconvert --type thin --thinpool $vg/pool1 --originname $lv2 $vg/$lv1
lvconvert --type thin --thinpool $vg/pool1 --originname $lv3 $vg/$lv1
lvconvert --type thin --thinpool $vg/pool1 --originname $lv4 $vg/$lv1

# External origins remain inactive
# (only their -real counterparts are in DM table)
check inactive $vg $lv2
check inactive $vg $lv3
check inactive $vg $lv4
check lv_attr_bit perm $vg/$lv2 "r"
check lv_attr_bit perm $vg/$lv3 "r"
check lv_attr_bit perm $vg/$lv4 "r"

lvchange -ay $vg

touch mnt/test2
sync
umount mnt

fsck -n "$DM_DEV_DIR/$vg/$lv1"
fsck -n "$DM_DEV_DIR/$vg/$lv2"
fsck -n "$DM_DEV_DIR/$vg/$lv3"
fsck -n "$DM_DEV_DIR/$vg/$lv4"

# Check the removal works fine
lvremove -f $vg


#####################################################
#
# Now retry with origins across multiple thin pools
#
#####################################################

_prepare

# Create several more thin pools
lvcreate -L10 -T $vg/pool2
lvcreate -L10 -T $vg/pool3
lvcreate -L10 -T $vg/pool4

# thin volume chained into 3 layers, each forming a new external origin

lvconvert --type thin --thinpool $vg/pool2 --originname $lv2 $vg/$lv1

lvconvert --type thin --thinpool $vg/pool3 --originname $lv3 $vg/$lv1

lvconvert --type thin --thinpool $vg/pool4 --originname $lv4 $vg/$lv1

# ATM we are not protecting against 'self-cycling'
lvconvert --type thin --thinpool $vg/pool1 --originname $lv5 $vg/$lv1

umount mnt
fsck -n "$DM_DEV_DIR/$vg/$lv1"

check active $vg $lv1
check inactive $vg $lv2
check inactive $vg $lv3
check inactive $vg $lv4
check inactive $vg $lv5
dm_table $vg-${lv4}-real | grep thin
dm_table $vg-${lv5}-real | grep thin

lvchange -ay $vg/$lv3

check active $vg $lv3

fsck -n "$DM_DEV_DIR/$vg/$lv3"

lvremove -f $vg





lvcreate -L10 -T $vg/pool1
lvcreate -an -pr -L1 -n $lv1 $vg

# Use linear as oring for multiple thins
lvcreate --type thin --thinpool $vg/pool1 $vg/$lv1 -n $lv2
lvcreate --type thin --thinpool $vg/pool1 $vg/$lv1 -n $lv3
lvcreate --type thin --thinpool $vg/pool1 $vg/$lv1 -n $lv4

# Convert existing $lv1 into external origin 'eorigin'
lvconvert -T --thinpool $vg/pool1 --originname eorigin $vg/$lv1

# check $lv1-real was converted into eorigin-real
not dmsetup info $vg-${lv1}-real &> out
grep "not exist" out
dm_table $vg-eorigin-real | grep linear

check inactive $vg $lv1
check inactive $vg eorigin
check active $vg $lv2
check active $vg $lv3
check active $vg $lv4

check lv_field $vg/$lv1 segtype thin
check lv_attr_bit perm $vg/$lv1 "r"

check lv_field $vg/eorigin segtype linear
check lv_attr_bit perm $vg/eorigin "r"

lvs -ao+uuid,segtype $vg

lvremove -f $vg




#####################################################
#
# Check some prohibited conversions
#
#####################################################

lvcreate -L10 -T -V20 -n $lv1 $vg/pool1

#lvcreate -L10 -T -V20 -n $lv2 $vg/pool2

# Take thick/old snapshot
lvcreate -s -L10 -n snap $vg/$lv1

# Converting old-snapshot into external origin is not supported
not lvconvert -T --thinpool $vg/pool1 --originname orig $vg/snap

# Converting thin-pools LVs
not lvconvert -T --thinpool $vg/pool1 --originname orig $vg/pool2
not lvconvert -T --thinpool $vg/pool1 --originname orig $vg/pool2_tmeta
not lvconvert -T --thinpool $vg/pool1 --originname orig $vg/pool2_tdata


# However we should be able to use 'thick' snapshot origin (which is thin)
lvconvert -T --thinpool $vg/pool1 --originname orig $vg/$lv1

check active $vg $lv1
check active $vg orig
check active $vg snap

check lv_field $vg/$lv1 segtype thin
check lv_field $vg/orig segtype thin
check lv_field $vg/snap segtype linear


lvchange -an $vg/orig

lvconvert --type thin --thinpool $vg/pool1 --originname orig2 $vg/orig

check active $vg $lv1
# external origin and snapshot stay inactive
check inactive $vg orig2
check inactive $vg orig
check inactive $vg snap

dm_table $vg-orig2-real | grep thin
