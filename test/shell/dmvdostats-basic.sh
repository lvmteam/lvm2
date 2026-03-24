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

# Test basic dmvdostats functionality.

. lib/inittest --skip-with-lvmpolld

aux have_vdo 9 0 0 || skip

aux lvmconf 'allocation/vdo_slab_size_mb = 128'

aux prepare_vg 1 7000
lvcreate --vdo -V3G -L4G -n $lv1 $vg/$lv2

VPOOL_DM="$vg-${lv2}-vpool"

# Enumerate all VDO devices (no args)
dmvdostats

# Single named device
dmvdostats "$VPOOL_DM"

# Verbose output — check key fields are present
dmvdostats -v "$VPOOL_DM" | tee verbose.out
grep -q "operating mode" verbose.out
grep -q "1K-blocks" verbose.out
grep -q "used percent" verbose.out
grep -q "saving percent" verbose.out

# Report with all fields
dmvdostats -o vdo_all "$VPOOL_DM"

# Report with selected fields
dmvdostats -o vdo_name,vdo_used_pct "$VPOOL_DM" | tee select.out
grep -q "$VPOOL_DM" select.out

# Via dmsetup subcommand
dmsetup vdostats "$VPOOL_DM"

# Non-VDO device should be rejected
not dmvdostats "$vg-$lv1"

vgremove -ff $vg
