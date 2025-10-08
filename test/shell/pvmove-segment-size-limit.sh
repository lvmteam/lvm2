#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description="test pvmove with pvmove_max_segment_size_mb limit"

. lib/inittest

# Test pvmove segment chunking with small sizes to avoid copying too much data
# We use 4MiB max segment size and 32MiB total volume size

aux prepare_pvs 4

get_devs

# Create VG with 4MiB extent size
vgcreate -s 4M "$vg" "${DEVICES[@]}"

# Create a 36MiB striped LV on dev1+2+3
# With 4MiB extent size, this is 3*9 extents
lvcreate -aey -L 36M -i3 -n "$lv" "$vg"

# Verify LV is on dev1+2+3
check lv_on "$vg" "$lv" "$dev1" "$dev2" "$dev3"

# Show initial segment layout
lvs -a -o name,size,seg_pe_ranges "$vg"

# Configure pvmove_max_segment_size to 4MiB
# This should split the 36MiB LV into 3 chunks of 12MiB (3 extent) each
aux lvmconf "allocation/pvmove_max_segment_size_mb = 4"

# Run pvmove with verbose logging to see the splitting
pvmove -i0 -vvv "$dev1" "$dev4" 2>&1 | tee pvmove.log

# Check that the splitting occurred
# We expect to see "Splitting" messages in the log
grep -q "Splitting" pvmove.log || die "Expected to see segment splitting in pvmove log"

# Count how many split operations occurred
# With 36MiB LV and 4MiB->12MiB limit, we expect 3 splits (to create 3 segments)
split_count=$(grep -c "Split segment.* at LE" pvmove.log || true)
echo "Number of splits: $split_count"

lvs -ao+seg_size,seg_pe_ranges $vg

# We should see 2 splits as from 4MiB we get 12MiB as stripe aligned chunks
test "$split_count" -eq 2 || die "Expected 2 splits, got $split_count"

# Verify that the max segment size message appears
grep -q "Pvmove max segment size.*4.* extent" pvmove.log || \
	die "Expected to see max segment size configuration in log"

# Verify LV is now on dev2+3+4
check lv_on "$vg" "$lv" "$dev2" "$dev3" "$dev4"

# Verify no pvmove LVs remain
get lv_field "$vg" name -a > out
not grep "^\[pvmove" out || die "pvmove LV still exists"

# Verify data integrity (LV should still be accessible)
lvchange -ay "$vg/$lv"
check lv_field "$vg/$lv" lv_active "active"

# Test with larger limit (no splitting expected)

# Configure with 64MiB limit (larger than our LV)
aux lvmconf "allocation/pvmove_max_segment_size_mb = 64"

# Run pvmove again
pvmove -i0 -vvv "$dev4" "$dev1" 2>&1 | tee pvmove-nosplit.log

# This time we should NOT see splitting
grep -q "within limit" pvmove-nosplit.log || \
	die "Expected to see 'within limit' message with large segment size"

# Should see zero splits
split_count_nosplit=$(grep -c "Split segment.* at LE" pvmove-nosplit.log || true)
echo "Number of splits with large limit: $split_count_nosplit"
test "$split_count_nosplit" -eq 0 || \
	die "Expected 0 splits with large limit, got $split_count_nosplit"

# Verify LV is on dev1+2+3
check lv_on "$vg" "$lv" "$dev1" "$dev2" "$dev3"

# Test with limit disabled (0 = unlimited)
aux lvmconf "allocation/pvmove_max_segment_size_mb = 0"

pvmove -i0 -vvv "$dev1" "$dev4" 2>&1 | tee pvmove-unlimited.log

# Should not see any splitting messages
not grep -q "Splitting" pvmove-unlimited.log

# Verify final state dev2+3+4
check lv_on "$vg" "$lv" "$dev2" "$dev3" "$dev4"

# Cleanup
vgremove -ff "$vg"
