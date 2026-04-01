#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test that lvreduce of a CoW snapshot COW store is rejected when the
# requested new size would truncate already-allocated exception blocks.
# https://github.com/lvmteam/lvm2/issues/164

. lib/inittest --skip-with-lvmpolld

aux prepare_vg

# Create a 16M origin and an 8M snapshot
lvcreate -L16M -n $lv1 $vg
lvcreate -L8M -s $vg/$lv1 -n $lv2

lvchange -an $vg
# Cannot reduce inactive snapshot.
not lvreduce --yes -L4M $vg/$lv2

lvchange -ay $vg

# Write 4M to the origin using direct I/O so the data bypasses the page
# cache and triggers CoW exception allocation in the snapshot immediately.
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=4 oflag=direct

# Snapshot must still be valid (not overflow)
check lv_attr_bit state $vg/$lv2 "a"

# Reducing to 4M must fail because ~4M of exception data is already
# allocated in the COW store (used_sectors includes metadata overhead).
not lvreduce --yes -L4M $vg/$lv2

# Snapshot must still be the original 8M
check lv_field $vg/$lv2 lv_size "8.00m"

# Overflow the snapshot COW store by writing directly to it until it is
# invalidated (kernel marks it 'I' when exceptions are exhausted).
not dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M oflag=direct

# Snapshot must now be invalid.
check lv_attr_bit state $vg/$lv2 "I"

# Reducing an invalid snapshot must be rejected regardless of size.
not lvreduce --yes -L4M $vg/$lv2

# Size must be unchanged.
check lv_field $vg/$lv2 lv_size "8.00m"

vgremove -f $vg
