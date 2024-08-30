#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test usability of 16g thin pool metadata  LV


SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_thin 1 0 0 || skip

aux prepare_vg 1 50000

lvcreate -T -L10 --poolmetadatasize 16g $vg/pool
check lv_field $vg/pool_tmeta size "<15.88g"
lvremove -f $vg

# Cropped way
lvcreate -T -L10 --poolmetadatasize 16g --config 'allocation/thin_pool_crop_metadata=1' $vg/pool
check lv_field $vg/pool_tmeta size "15.81g"
lvremove -f $vg

lvcreate -L16G -n meta $vg
lvcreate -L10  -n pool $vg
lvconvert --yes --thinpool $vg/pool --poolmetadata meta
# Uncropped size 33554432 sectors - 16GiB
dmsetup table ${vg}-pool_tmeta | grep 33554432
lvremove -f $vg

# Uses 20G metadata volume, but crops the size in DM table
lvcreate -L20G -n meta $vg
lvcreate -L10  -n pool $vg
lvconvert --yes --thinpool $vg/pool --poolmetadata meta --config 'allocation/thin_pool_crop_metadata=1'
check lv_field $vg/lvol0_pmspare size "16.00g"
# Size should be cropped to 33161216 sectors  ~15.81GiB
dmsetup table ${vg}-pool_tmeta | grep 33161216

# Also size remains unchanged with activation has no cropping,
# but metadata have no CROP_METADATA flag set
lvchange -an $vg
lvchange -ay $vg
# Size still stays cropped to 33161216 sectors  ~15.81GiB
dmsetup table ${vg}-pool_tmeta | grep 33161216
lvremove -f $vg

# Minimal size is 2M
lvcreate -L1M -n meta $vg
lvcreate -L10 -n pool $vg
not lvconvert --yes --thinpool $vg/pool --poolmetadata meta
lvremove -f $vg

# Uses 20G metadata volume, but crops the size in DM table
lvcreate -L1 --poolmetadatasize 10G -T $vg/pool
lvresize -L+10G $vg/pool_tmeta --config 'allocation/thin_pool_crop_metadata=1'
check lv_field $vg/lvol0_pmspare size "15.81g"
# Size should be cropped to 33161216 sectors  ~15.81GiB
dmsetup table ${vg}-pool_tmeta | grep 33161216

# Without cropping we can grow to ~15.88GiB
lvresize -L+10G $vg/pool_tmeta
check lv_field $vg/lvol0_pmspare size "<15.88g"
lvremove -f $vg

# User has already 'bigger' metadata and wants them uncropped
lvcreate -L16G -n meta $vg
lvcreate -L10  -n pool $vg
lvconvert --yes --thinpool $vg/pool --poolmetadata meta --config 'allocation/thin_pool_crop_metadata=1'

# No change with cropping
lvresize -l+1 $vg/pool_tmeta --config 'allocation/thin_pool_crop_metadata=1'
dmsetup table ${vg}-pool_tmeta | grep 33161216

# Resizes to 'uncropped' size 16GiB with ANY size
lvresize -l+1 $vg/pool_tmeta
dmsetup table ${vg}-pool_tmeta | grep 33554432
check lv_field $vg/pool_tmeta size "16.00g"

vgremove -ff $vg
