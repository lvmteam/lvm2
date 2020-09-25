#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test various error conditions user may hit with thin volumes

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

#
# Main
#
aux have_thin 1 3 0 || skip
aux thin_pool_error_works_32 || skip

aux prepare_vg 2

###############################################
#  Testing failing thin-pool metadata device  #
###############################################

lvcreate -T -L1M --errorwhenfull y $vg/pool
lvcreate -V2 -n $lv1 $vg/pool
lvcreate -s -n $lv2 $vg/$lv1

# Prepare old metadata with transaction_id 2
vgcfgbackup -f mda_tid_2 $vg

lvcreate -s -n $lv3 $vg/$lv1
lvcreate -s -n $lv4 $vg/$lv1
lvcreate -s -n $lv5 $vg/$lv1

vgcfgbackup -f mda_tid_5 $vg

# Restore mismatching old metadata with different transaction_id
vgcfgrestore -f mda_tid_2 --force --yes $vg


not lvcreate -s -n $lv5 $vg/$lv1

sed -e 's/transaction_id = 2/transaction_id = 5/g' mda_tid_2 > mda_tid_2_5

# Restore metadata with matching transaction_id, 
# but already existing device in kernel, unknown to lvm2
vgcfgrestore -f mda_tid_2_5 --force --yes $vg

not lvcreate -s -n $lv5 $vg/$lv1
# can be tried repeatedly
not lvcreate -s -n $lv5 $vg/$lv1


# Restore matching metadata and check all works
# and no kernel thin device was lost
vgcfgrestore -f mda_tid_5 --force --yes $vg

lvcreate -s -n $lv6 $vg/$lv1

lvchange -ay -K $vg

check active $vg $lv1
check active $vg $lv2
check active $vg $lv3
check active $vg $lv4
check active $vg $lv5
check active $vg $lv6

vgremove -ff $vg
