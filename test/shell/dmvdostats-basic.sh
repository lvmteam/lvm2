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

aux prepare_vg 1 12000

lvcreate --vdo -V3G -L4G -n $lv1 $vg/$lv2

VPOOL_DM="$vg-${lv2}-vpool"

# Enumerate all VDO devices (no args)
dmvdostats

# Single named device
dmvdostats "$VPOOL_DM"

# Verbose output - check key fields are present
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

# help -c must list VDO report fields, not dmstats fields
dmvdostats -h -c 2>&1 | tee help-c.out
grep -q vdo_physical_size help-c.out
not grep -q reads_merged_count help-c.out

# Via dmsetup subcommand
dmsetup vdostats "$VPOOL_DM"

# A VDO LV layered above the pool resolves down to the VDO pool device
dmvdostats "$vg-$lv1" | tee lv.out
grep -q "$VPOOL_DM" lv.out

# ...and it works through the /dev path as well
dmvdostats "$DM_DEV_DIR/$vg/$lv1" | tee lvpath.out
grep -q "$VPOOL_DM" lvpath.out

# Naming the VDO pool LV without the -vpool suffix falls back to the pool device
dmvdostats "$vg-$lv2" | tee vpool.out
grep -q "$VPOOL_DM" vpool.out

# Dependency-tree walk: skip invalid sysfs-derived names (e.g. '#') and descend.
INVALID="${PREFIX}vdostats-bad#mid"
TOP="${PREFIX}vdostats-top"
read -r vsize < <(dmsetup table "$VPOOL_DM" | awk '{print $2; exit}')
vmajor=$(dmsetup info -c --noheadings -o major "$VPOOL_DM")
vminor=$(dmsetup info -c --noheadings -o minor "$VPOOL_DM")
dmsetup create "$INVALID" --verifyudev --manglename none \
	--table "0 $vsize linear $vmajor:$vminor 0"
bmajor=$(dmsetup info -c --noheadings -o major "$INVALID")
bminor=$(dmsetup info -c --noheadings -o minor "$INVALID")
dmsetup create "$TOP" --table "0 $vsize linear $bmajor:$bminor 0"
dmvdostats "$TOP" | tee invalid-name-walk.out
grep -q "$VPOOL_DM" invalid-name-walk.out
dmsetup vdostats "$TOP" | tee invalid-name-walk-dmsetup.out
grep -q "$VPOOL_DM" invalid-name-walk-dmsetup.out
dmsetup remove "$TOP"
dmsetup remove "$INVALID" --verifyudev --manglename none

# A plain LV with no underlying VDO device is rejected
lvcreate -L1 -n $lv3 $vg
not dmvdostats "$vg-$lv3"

# A non-existent device is reported as not found
not dmvdostats "${PREFIX}nosuchdevice"

lvremove -ff $vg

# Check if multiple vdopool are found and lists in stack
lvcreate -T --pooldatavdo y -L5G -V1G $vg/pool1 -n $lv1
# Make $lv1  read-only inactive LV for external origin
lvchange -p r $vg/$lv1
lvchange -an $vg/$lv1
lvcreate -T --pooldatavdo y -L5G $vg/pool2
lvcreate --snapshot -n $lv2 --thinpool $vg/pool2 $vg/$lv1

dmvdostats "$vg-$lv2" | tee vpool.out
grep -q "pool1_vpool" vpool.out
grep -q "pool2_vpool" vpool.out

vgremove -ff $vg
