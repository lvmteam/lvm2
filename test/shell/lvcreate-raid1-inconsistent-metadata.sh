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

# Test lvcreate raid1 behavior with inconsistent metadata after device failures
# Regression test for https://issues.redhat.com/browse/RHEL-116884
#
# Scenario:
# 1. Create mirror volume with nosync across multiple PVs
# 2. Simulate transient device failures (3 out of 4 devices go offline)
# 3. Bring devices back online - this creates metadata inconsistency
# 4. Attempt to create a new raid1 volume - should fail before vgck --updatemetadata
# 5. After vgck --updatemetadata, lvcreate should work correctly

. lib/inittest

aux have_raid 1 3 0 || skip

aux prepare_vg 4 10

# Create initial mirror volume with nosync using all free space in VG
lvcreate --yes --type mirror -n mirror_LV --nosync -l100%VG -m 3 $vg

# Verify mirror was created successfully
check lv_exists $vg mirror_LV
lvs -a $vg

# Deactivate the mirror
lvchange -an $vg/mirror_LV

# Simulate device failures - disable 3 out of 4 devices
aux disable_dev "$dev1" "$dev2" "$dev4"

# The VG should report missing PVs
not pvs "$dev1"  "$dev2" "$dev3" "$dev4" 2>&1 | tee out
vgs $vg 2>&1 | tee out
grep -i "missing" out

# Try to fix metadata (like if dmeventd would applied repair policy)
lvconvert --repair --use-policies $vg/mirror_LV

# Deactivate VG while devices are missing
vgchange -an $vg

# Bring all devices back online
aux enable_dev "$dev1" "$dev2" "$dev4"

# This creates metadata inconsistency - some devices have old metadata
vgs $vg 2>&1 | tee out
grep -i "Ignoring metadata" out
grep -i "Inconsistent metadata" out
grep -i "unused reappeared PV" out

# Activate VG with inconsistent metadata
vgchange -ay $vg

# Attempt to create raid1 volume
# (used to create corrupted empty LV segments - RHEL-116884)
not lvcreate --yes --type raid1 -n raid_LV --nosync -l100%FREE -m 3 $vg 2>&1 | tee raid_fail.out

# Check that it failed with insufficient extents
grep -i "Insufficient suitable allocatable extents" raid_fail.out

# The LV should not exist
not check lv_exists $vg raid_LV

# Run vgck --updatemetadata to fix inconsistent metadata
vgck --updatemetadata $vg 2>&1 | tee vgck.out

# Should see metadata being updated
grep -i "Updating old metadata" vgck.out

# After vgck, metadata should be consistent
vgs $vg 2>&1 | tee out
not grep -i "Inconsistent metadata" out
not grep -i "unused reappeared PV" out

# Only the original mirror should exist
lvs -ao+seg_pe_ranges $vg | tee out
grep mirror_LV out
not grep raid_LV out

# Reduce mirror_LV size so there is some usable free space to create raid_LV
lvreduce --yes -l10%VG $vg/mirror_LV

# Now lvcreate raid1 should succeed after metadata is fixed
lvcreate --yes --type raid1 -n raid_LV --nosync -l100%FREE -m 3 $vg

# Verify raid1 was created successfully
check lv_exists $vg raid_LV
check lv_field $vg/raid_LV segtype "raid1"

# Verify all sub-LVs have proper segments
lvs -a $vg 2>&1 | tee final.out
not grep -i "Internal error" final.out
not grep -i "has no segment" final.out

vgremove -ff $vg
