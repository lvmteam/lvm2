#!/usr/bin/env bash

# Copyright (C) 2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Exercise caching vdo and vdo-pool's data LV


SKIP_WITH_LVMPOLLD=1

. lib/inittest

#
# Main
#

#
# FIXME: tempororarily disable this test until fixed VDO driver is relased
#        should really be 6.2.2 - currently goes with vdo-6.2.2.18
aux have_vdo 6 2 1 || skip
aux have_cache 1 3 0 || skip

which mkfs.ext4 || skip
export MKE2FS_CONFIG="$TESTDIR/lib/mke2fs.conf"

aux prepare_vg 1 9000

lvcreate --vdo -L4G -V2G --name $lv1 $vg/vpool

# Test caching VDOPoolLV
lvcreate -H -L10 $vg/vpool

mkfs.ext4 -E nodiscard "$DM_DEV_DIR/$vg/$lv1"

lvconvert --uncache $vg/vpool
fsck -n "$DM_DEV_DIR/$vg/$lv1"

lvcreate -H -L10 $vg/vpool_vdata
fsck -n "$DM_DEV_DIR/$vg/$lv1"
lvs -a $vg
lvconvert --uncache $vg/vpool_vdata


# Test caching VDOLV
lvcreate -H -L10 $vg/$lv1

lvconvert --uncache $vg/$lv1
fsck -n "$DM_DEV_DIR/$vg/$lv1"

lvs -a $vg

vgremove -ff $vg
