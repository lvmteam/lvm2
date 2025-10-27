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

# Test for classic snapshots of raid throwing warning/error on creation/activation


. lib/inittest --skip-with-lvmpolld

aux have_raid 1 3 0 || skip

aux prepare_devs 2 50

# Create Raid1LV and wait for initial sync to finish.
vgcreate $SHARED $vg "$dev1" "$dev2"
lvcreate -y --type raid1 -m1 -n $lv1 -L32M $vg
aux wait_for_sync $vg $lv1

# Create classic snapshot of Raid1LV and check for warning message.
lvcreate -y -s -n snap -L12M $vg/$lv1 "$dev1" 2>&1 | grep 'WARNING: Loss of snapshot ' >/dev/null 2>&1

# Disable all LVs and the first PV with the snapshot allocated on.
vgchange -an $vg
aux disable_dev "$dev1"

# Active vg and check for warning message.
not vgchange -ay $vg 2>&1 | grep -E 'Activating raid LV .* requires the removal of partial snapshot' >/dev/null 2>&1

# Remove the snapshot having lost its backing PV.
lvremove -y $vg/snap

# Active vg and check for warning message not being displayed.
vgchange -ay $vg 2>&1 | not grep -E 'Activating raid LV .* requires the removal of partial snapshot' >/dev/null 2>&1

ls $DM_DEV_DIR/$vg/$lv1

lvremove -y $vg/$lv1
vgremove -ff $vg
