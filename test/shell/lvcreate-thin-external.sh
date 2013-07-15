#!/bin/sh

# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Test creation of thin snapshots using external origin

. lib/test

which mkfs.ext2 || skip
which fsck || skip

#
# Main
#
aux have_thin 1 3 0 || skip

aux prepare_pvs 2 64

vgcreate $vg -s 64K $(cat DEVICES)

lvcreate -L10M -V10M -T $vg/pool --name $lv1
mkfs.ext2 $DM_DEV_DIR/$vg/$lv1

lvcreate -L4M -n $lv2 $vg
mkfs.ext2 $DM_DEV_DIR/$vg/$lv2

# Fail to create external origin snapshot of rw LV
not lvcreate -s $vg/$lv2 --thinpool $vg/pool

lvchange -p r $vg/$lv2

# Fail to create snapshot of active r LV
# FIXME: kernel update needed
not lvcreate -s $vg/$lv2 --thinpool $vg/pool

# Deactivate LV we want to use as external origin
# once kernel will ensure read-only this condition may go away
lvchange -an $vg/$lv2

lvcreate -s $vg/$lv2 --thinpool $vg/pool

# Fail with --thin and --snapshot
not lvcreate -s $vg/$lv5 --name $vg/$lv7 -T $vg/newpool

# Fail to create already existing pool
not lvcreate -s $vg/$lv2 -L10 --thinpool $vg/pool
not lvcreate -s $vg/$lv2 --chunksize 64 --thinpool $vg/pool
not lvcreate -s $vg/$lv2 --zero y --thinpool $vg/pool
not lvcreate -s $vg/$lv2 --poolmetadata $vg/$lv1 --thinpool $vg/pool

# Fail with nonexistent pool
not lvcreate -s $vg/$lv2 --thinpool $vg/newpool

# Create pool and snap
lvcreate -s -K $vg/$lv2 --name $vg/$lv3 -L20 --chunksize 128 --thinpool $vg/newpool
lvcreate -s -K $vg/$lv3 --name $vg/$lv4
lvcreate -s -K $vg/$lv2 --name $vg/$lv5 --thinpool $vg/newpool
# Make normal thin snapshot
lvcreate -s -K $vg/$lv5 --name $vg/$lv6
# We do not need to specify thinpool when doing thin snap, but it should work
lvcreate -s -K $vg/$lv5 --name $vg/$lv7 --thinpool $vg/newpool

check inactive $vg $lv2
lvchange -ay $vg/$lv2
lvcreate -s -K $vg/$lv2 --name $vg/$lv8 --thinpool $vg/newpool

lvs -o+chunksize $vg

check active $vg $lv3
check active $vg $lv4
check active $vg $lv5
check active $vg $lv6
check active $vg $lv7

fsck -n $DM_DEV_DIR/$vg/$lv1
fsck -n $DM_DEV_DIR/$vg/$lv7

vgremove -ff $vg
