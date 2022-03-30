#!/usr/bin/env bash

# Copyright (C) 2022 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#
# Play with thin-pool and thin removal and creation in corner cases
#

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

aux have_thin 1 0 0 || skip

test -n "$LVM_TEST_THIN_RESTORE_CMD" || LVM_TEST_THIN_RESTORE_CMD=$(which thin_restore) || skip
"$LVM_TEST_THIN_RESTORE_CMD" -V || skip

aux have_thin 1 10 0 || skip

aux prepare_vg 2

lvcreate -V10 -n $lv1 -L10 -T $vg/pool
lvcreate -V10 -n $lv2 $vg/pool

# Forcibly 'error' _tmeta thin-pool metadata device
not dmsetup remove -f $vg-pool_tmeta

# Now try to schedule removal of thin volume id 1
# that will fail with errored meta device
not lvremove -y $vg/$lv1

# Check we have queued 'message'
vgcfgbackup -f out0 $vg
grep "message1" out0

vgchange -an $vg || true

not dmsetup table ${vg}-pool-tpool

# Reactivate thin-pool
vgchange -ay $vg

# Check message is still queued there
vgcfgbackup -f out1 $vg
grep "message1" out1

lvchange -an $vg

lvextend -L+10 $vg/pool

# Messages should be now processed and gone
vgcfgbackup -f out2 $vg
not grep "message1" out2

lvchange -an $vg

lvchange -y -ay $vg/pool_tmeta

# Kernel metadata must not see dev_id 1 either
thin_dump $DM_DEV_DIR/$vg/pool_tmeta | tee meta
not grep 'dev_id="1"' meta

lvremove -ff $vg

lvs -a $vg

vgremove -ff $vg
