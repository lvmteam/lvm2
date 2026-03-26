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

# Test VDO compression: write compressible data (~74% compressible for ~3:1
# packing), verify physical usage is approximately 1/3 of logical blocks.
# Then write the same data to a new offset to verify dedupe of compressed
# data. Finally trim both ranges and verify space is fully reclaimed.
# Derived from VDOTest::Compress01 in vdo-devel.

. lib/inittest --skip-with-lvmpolld --with-extended

aux have_vdo 6 2 0 || skip
which blkdiscard || skip

aux prepare_vg 1 6400

lvcreate --vdo -L5G -V10G -n $lv1 $vg/vdopool --compression y

VPOOL="$vg-vdopool-vpool"
aux wait_for_vdo_index "$VPOOL"

DEV="$DM_DEV_DIR/$vg/$lv1"
BLOCKS=5000
BSIZE=4096

# Verify initial state
blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq 0

# Write compressible data (~74% compressible => ~3:1 packing)
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" \
	--data=d1,0,0.74 writeSlice
sync
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" \
	--data=d1,0,0.74 verifySlice

# Physical usage should be approximately 1/3 of blocks written.
# Wide tolerance (+-33%) accounts for VDO metadata overhead, packer
# bin fragmentation, and per-block header bytes reducing effective
# compression slightly below the nominal 74% target.
blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
expected=$(( BLOCKS / 3 ))
tolerance=$(( expected / 3 ))
test "$blocks_used" -gt $(( expected - tolerance ))
test "$blocks_used" -lt $(( expected + tolerance ))

saved_blocks="$blocks_used"

# Write same compressible data at new offset -- should fully dedupe
gen_data_blocks --device="$DEV" --blockCount="$BLOCKS" \
	--offset="$BLOCKS" --data=d1,0,0.74 writeSlice
sync

blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq "$saved_blocks"

# Trim both ranges -- all blocks should be reclaimed
blkdiscard -o 0 -l $((BLOCKS * BSIZE)) "$DEV"
blkdiscard -o $((BLOCKS * BSIZE)) -l $((BLOCKS * BSIZE)) "$DEV"
sync

blocks_used=$(aux get_vdo_stat "$VPOOL" "data blocks used")
test "$blocks_used" -eq 0

vgremove -ff $vg
