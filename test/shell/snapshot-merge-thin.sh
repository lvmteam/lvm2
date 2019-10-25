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

# Exercise merge of old snapshots over thin

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

aux target_at_least dm-snapshot-merge 1 0 0 || skip
aux have_thin 1 0 0 || skip

aux prepare_vg 2

lvcreate -T -L1 -V1 -n $lv1 $vg/pool "$dev1"
lvcreate -s -n $lv2 -L2 $vg/$lv1 "$dev2"
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M count=1 conv=fdatasync

# Initiate background merge
lvconvert -b --merge $vg/$lv2

# Query status of snapshot immediatelly after start
# - may hit race of checking already in-progress merge
lvs -a -o+lv_merging,lv_merge_failed $vg

sleep 1

# Here should be everything already merged
lvs -a -o+lv_merging,lv_merge_failed $vg

# -real must not exist for  $vg/$lv1
not dmsetup info ${vg}-${lv1}-real 2>&1 | tee out
grep "not exist" out

not dmsetup info ${vg}-${lv2}-cow 2>&1 | tee out
grep "not exist" out

check lv_not_exists $vg $lv2

vgremove -f $vg
