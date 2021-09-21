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

# Test how zeroing of thin-pool metadata works

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

#
# Main
#
aux have_thin 1 3 0 || skip
aux have_cache 1 3 0 || skip

aux prepare_vg 3 40000

# Create mostly-zero devs only front of it has some 'real' back-end
aux zero_dev "$dev1" "$(( $(get first_extent_sector "$dev1") + 8192 )):"
aux zero_dev "$dev2" "$(( $(get first_extent_sector "$dev2") + 8192 )):"
aux zero_dev "$dev3" "$(( $(get first_extent_sector "$dev3") + 8192 )):"

# Prepare randomly filled 4M LV on dev2
lvcreate -L16G -n $lv1 $vg "$dev2"
dd if=/dev/urandom of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=4 oflag=direct  || true
lvremove -f $vg

for i in 0 1
do
	aux lvmconf "allocation/zero_metadata = $i"

	# Lvm2 should allocate metadata on dev2
	lvcreate -T -L10G --poolmetadatasize 16G $vg/pool "$dev1" "$dev2"
	lvchange -an $vg

	lvs -ao+seg_pe_ranges $vg
	lvchange -ay $vg/pool_tmeta --yes

	# Skip past 1.2M which is 'created' by  thin-pool initialization
	hexdump -C -n 200 -s 2000000 "$DM_DEV_DIR/$vg/pool_tmeta" | tee out

	# When fully zeroed, it should be zero - so almost no output from hexdump
	case "$i" in
	0) test "$(wc -l < out)" -ge 10 ;; # should not be zeroed
	1) test "$(wc -l < out)" -le 10 ;; # should be zeroed
	esac

	lvremove -f $vg/pool
done

# Check lvm2 spots error during full zeroing of metadata device
aux error_dev "$dev2" "$(( $(get first_extent_sector "$dev2") + 32 )):"
not lvcreate -T -L10G --poolmetadatasize 16G $vg/pool "$dev1" "$dev2" |& tee err
grep "Failed to initialize logical volume" err

vgremove -ff $vg
