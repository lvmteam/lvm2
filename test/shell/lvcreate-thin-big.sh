#!/bin/sh

# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# test currently needs to drop
# 'return NULL' in _lv_create_an_lv after log_error("Can't create %s without using "

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux have_thin 1 0 0 || skip

# Test --poolmetadatasize range
# allocating large devices for testing
aux prepare_pvs 10 16500
vgcreate $vg -s 64K $(cat DEVICES)

# Size 0 is not valid
invalid lvcreate -L4M --chunksize 128 --poolmetadatasize 0 -T $vg/pool1 2>out
lvcreate -L4M --chunksize 128 --poolmetadatasize 16k -T $vg/pool1 2>out
grep "WARNING: Minimum" out
# FIXME: metadata allocation fails, if PV doesn't have at least 16GB
# i.e. pool metadata device cannot be multisegment
lvcreate -L4M --chunksize 64k --poolmetadatasize 17G -T $vg/pool2 2>out
grep "WARNING: Maximum" out
check lv_field $vg/pool1_tmeta size "2.00m"
check lv_field $vg/pool2_tmeta size "16.00g"
lvremove -ff $vg

# Test automatic calculation of pool metadata size
lvcreate -L160G -T $vg/pool
check lv_field $vg/pool lv_metadata_size "80.00m"
check lv_field $vg/pool chunksize        "128.00k"
lvremove -ff $vg/pool

lvcreate -L10G --chunksize 256 -T $vg/pool1
lvcreate -L60G --chunksize 1024 -T $vg/pool2
check lv_field $vg/pool1_tmeta size "2.50m"
check lv_field $vg/pool2_tmeta size "3.75m"
lvremove -ff $vg

# Block size of multiple 64KB needs >= 1.4
if aux have_thin 1 4 0 ; then
# Test chunk size is rounded to 64KB boundary
lvcreate -L10G --poolmetadatasize 4M -T $vg/pool
check lv_field $vg/pool chunk_size "192.00k"
fi
# Old thinpool target required rounding to power of 2
aux lvmconf "global/thin_disabled_features = [ \"block_size\" ]"
lvcreate -L10G --poolmetadatasize 4M -T $vg/pool_old
check lv_field $vg/pool_old chunk_size "256.00k"
lvremove -ff $vg
# reset
#aux lvmconf "global/thin_disabled_features = []"

vgremove -ff $vg
