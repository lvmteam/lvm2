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

# Test VDO rebuild: write known-good data to VDO, then simulate a dirty
# shutdown using dm-flakey with drop_writes so VDO's shutdown I/O is
# silently lost. On reactivation VDO must detect the dirty state, run
# recovery, and the previously fsynced data must survive intact.
# Derived from VDOTest::DoryRebuildFast in vdo-devel.

. lib/inittest --skip-with-lvmpolld --with-extended

aux have_vdo 6 2 0 || skip

aux prepare_vg 1 6400

lvcreate --vdo -L5G -V10G -n $lv1 $vg/vdopool

VPOOL="$vg-vdopool-vpool"
aux wait_for_vdo_index "$VPOOL"

DEV="$DM_DEV_DIR/$vg/$lv1"

#
# Phase 1: Write and sync known-good data
#
gen_data_blocks --device="$DEV" --blockCount=2000 --data=good,0.1 \
	--fsync writeSlice
gen_data_blocks --device="$DEV" --blockCount=2000 --data=good,0.1 \
	verifySlice

#
# Phase 2: Simulate dirty shutdown with dm-flakey drop_writes
#
# Swap the PV to dm-flakey with drop_writes -- reads pass through but
# writes are silently dropped. Any data VDO writes from here on,
# including its shutdown journal and superblock flush, never reaches disk.
aux dropwrites_dev "$dev1"

# Write expendable data -- VDO thinks it succeeds but nothing persists
gen_data_blocks --device="$DEV" --blockCount=5000 --offset=2000 \
	--data=crash writeSlice

#
# Phase 3: Tear down and recover
#
# lvchange -an does a clean LVM stack teardown, but VDO's shutdown writes
# are silently dropped by flakey, so the on-disk state is dirty.
lvchange -an $vg/$lv1
lvchange -an $vg/vdopool

# Restore PV to normal linear -- now the disk has pre-crash metadata
aux enable_dev "$dev1"

# Reactivate -- VDO detects dirty shutdown and runs recovery
lvchange -ay $vg/vdopool
lvchange -ay $vg/$lv1
aux wait_for_vdo_index "$VPOOL"

#
# Phase 4: Verify previously committed data survived recovery
#
gen_data_blocks --device="$DEV" --blockCount=2000 --data=good,0.1 \
	verifySlice

# The crash data was never fsynced before the dirty shutdown, so VDO
# is not required to preserve it.  Verify the region is not the crash
# pattern -- it should be either zero or garbage, not intact "crash" data.
not gen_data_blocks --device="$DEV" --blockCount=5000 --offset=2000 \
	--data=crash verifySlice

vgremove -ff $vg
