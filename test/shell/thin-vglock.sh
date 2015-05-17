#!/bin/sh
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Test locking works and doesn't update metadata
# RHBZ: https://bugzilla.redhat.com/show_bug.cgi?id=1063542

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

MKFS=mkfs.ext2
which $MKFS || skip

aux have_thin 1 0 0 || skip
aux prepare_vg

lvcreate -L10 -T -V5 -n $lv1 $vg/pool
lvcreate -an -V10 -T $vg/pool

$MKFS "$DM_DEV_DIR/$vg/$lv1"
mkdir mnt
mount "$DM_DEV_DIR/$vg/$lv1" mnt

lvcreate -s -n snap $vg/$lv1
check lv_field $vg/snap thin_id "3"

lvconvert --merge $vg/snap

umount mnt
vgchange -an $vg

# Check reboot case
vgchange -ay --sysinit $vg
# Metadata are still not updated (--poll n)
check lv_field $vg/$lv1 thin_id "1"
check lv_field $vg/pool transaction_id "3"

# Check the metadata are updated after refresh
vgchange --refresh $vg
check lv_field $vg/$lv1 thin_id "3"
check lv_field $vg/pool transaction_id "4"

#lvs -a -o+transaction_id,thin_id $vg

vgremove -f $vg
