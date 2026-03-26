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

# Test VDO deduplication: write unique blocks, write the same data at a
# different offset, and verify VDO deduplicates them. Also verify that
# trim reclaims physical space correctly when deduplicated blocks are freed.
# Derived from VDOTest::Direct06 and VDOTest::Direct04 in vdo-devel.

. lib/inittest --skip-with-lvmpolld --with-extended

aux have_vdo 6 2 0 || skip
which blkdiscard || skip

aux prepare_vg 1 6400

lvcreate --vdo -L5G -V10G -n $lv1 $vg/vdopool

VPOOL="$vg-vdopool-vpool"
aux wait_for_vdo_index "$VPOOL"

DEV="$DM_DEV_DIR/$vg/$lv1"
BLOCKS=5000
BSIZE=4096

# Verify initial state: no data blocks used
blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq 0

# Write 5000 unique blocks tagged "d1"
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" --data=d1 writeSlice
sync
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" --data=d1 verifySlice

blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq "$BLOCKS"

# Write the same data at a different offset -- should fully dedupe
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" --offset="$BLOCKS" \
	--data=d1 writeSlice
sync

blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq "$BLOCKS"

# Both ranges should still verify
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" --data=d1 verifySlice
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" --offset="$BLOCKS" \
	--data=d1 verifySlice

# Restart the device and verify data persists
lvchange -an $vg/$lv1
lvchange -an $vg/vdopool
lvchange -ay $vg/vdopool
lvchange -ay $vg/$lv1
aux wait_for_vdo_index "$VPOOL"

gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" --data=d1 verifySlice
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" --offset="$BLOCKS" \
	--data=d1 verifySlice

# Trim first range -- blocks still referenced by second copy
blkdiscard -o 0 -l $((BLOCKS * BSIZE)) "$DEV"
sync

blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq "$BLOCKS"

# Trim second range -- all physical blocks should be freed
blkdiscard -o $((BLOCKS * BSIZE)) -l $((BLOCKS * BSIZE)) "$DEV"
sync

blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq 0

vgremove -ff $vg
