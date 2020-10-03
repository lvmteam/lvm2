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
lvcreate -s -n $lv2 $vg/$lv1

# Take also thick snapshot of thin snapshot
lvcreate -s -L1 -n $lv3 $vg/$lv2

sleep 10 < "$DM_DEV_DIR/$vg/$lv1" >/dev/null 2>&1 &
PID_SLEEP=$!

# initiated merge that cannot proceed, but there is no need to retry
lvconvert --config 'activation/retry_deactivation=0' --merge $vg/$lv2

kill $PID_SLEEP
wait

# Remove everything
lvremove --yes $vg

# No LV left in VG
check  vg_field $vg  lv_count "0"


# Create again pool with thin and thick snapshot
lvcreate -T -L1 -V1 -n $lv1 $vg/pool "$dev1"
lvcreate -s -n $lv2 -L2 $vg/$lv1 "$dev2"
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M count=1 oflag=direct

lvs -a -o+lv_merging,lv_merge_failed $vg
aux delay_dev "$dev1" 0 400 "$(get first_extent_sector "$dev1"):"

# Initiate background merge
lvconvert -b --merge $vg/$lv2

# Query status of snapshot immediately after start
# - may hit race of checking already in-progress merge
#lvs -a -o+lv_merging,lv_merge_failed $vg
check lv_field $vg/$lv1 lv_merging "merging"

lvm lvpoll -i 1 --polloperation merge $vg/$lv1

# Here should be everything already merged
#lvs -a -o+lv_merging,lv_merge_failed $vg
# check we see thin filled 100%  (1MiB written to 1MiB LV)
check lv_field $vg/$lv1 data_percent "100.00"

# -real must not exist for  $vg/$lv1
not dmsetup info ${vg}-${lv1}-real 2>&1 | tee out
grep "not exist" out

not dmsetup info ${vg}-${lv2}-cow 2>&1 | tee out
grep "not exist" out

check lv_not_exists $vg $lv2

vgremove -f $vg
