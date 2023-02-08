#!/usr/bin/env bash

# Copyright (C) 2008-2013,2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_vg 3

lvcreate -n $lv1 -L8M --type mirror -m 1 $vg
lvcreate -n $lv2 -L8M --type mirror -m 1 $vg

vgchange -an $vg

pvs
vgs
lvs -a -o+devices

# Fail one leg of each mirror LV.
aux disable_dev "$dev1"

pvs -o+missing |tee out
grep missing out |tee out2
grep unknown out2
vgs -o+partial,missing_pv_count
check vg_field $vg vg_partial "partial"
check vg_field $vg vg_missing_pv_count 1
lvs -a -o+devices

# Cannot do normal activate of either LV with a failed leg.
not lvchange -ay $vg/$lv1
not lvchange -ay $vg/$lv2

# Can activate with partial option.
lvchange -ay --activationmode partial $vg/$lv1
lvchange -ay --activationmode partial $vg/$lv2

pvs -o+missing |tee out
grep missing out |tee out2
grep unknown out2
vgs -o+partial,missing_pv_count
check vg_field $vg vg_partial "partial"
check vg_field $vg vg_missing_pv_count 1
lvs -a -o+devices

# Repair lv1 so it no longer uses failed dev.
lvconvert --repair --yes $vg/$lv1

# Check that MISSING flag is set in ondisk metadata,
# it should have been written by the lvconvert since the
# missing PV is still used by lv2.
pvck --dump metadata "$dev2" > meta
grep MISSING meta
rm meta

pvs -o+missing |tee out
grep missing out |tee out2
grep unknown out2
vgs -o+partial,missing_pv_count
check vg_field $vg vg_partial "partial"
check vg_field $vg vg_missing_pv_count 1
lvs -a -o+devices

# Verify normal activation is possible of lv1 since it's
# not using any failed devs, and partial activation is
# required for lv2 since it's still using the failed dev.
vgchange -an $vg
lvchange -ay $vg/$lv1
not lvchange -ay $vg/$lv2
vgchange -an $vg

aux enable_dev "$dev1"

pvs -o+missing |tee out
grep missing out |tee out2
grep "$dev1" out2
vgs -o+partial,missing_pv_count
check vg_field $vg vg_partial "partial"
check vg_field $vg vg_missing_pv_count 1
lvs -a -o+devices

# TODO: check that lv2 has partial flag, lv1 does not
# (there's no partial reporting option, only attr p.)

# Check that MISSING flag is still set in ondisk
# metadata since the previously missing dev is still
# used by lv2.
pvck --dump metadata "$dev2" > meta
grep MISSING meta
rm meta

# The missing pv restrictions still apply even after
# the dev has reappeared since it has the MISSING flag.
not lvchange -ay $vg/$lv2
not lvcreate -l1 $vg

# Update old metadata on the previously missing PV.
# This should not clear the MISSING flag because the
# previously missing PV is still used by lv2.
# This would be done by any command that writes
# metadata, e.g. lvcreate, but since we are in a
# state with a missing pv, most commands that write
# metadata are restricted, so use a command that
# explicitly writes/fixes metadata.
vgck --updatemetadata $vg

pvs -o+missing |tee out
grep missing out |tee out2
grep "$dev1" out2
vgs -o+partial,missing_pv_count
check vg_field $vg vg_partial "partial"
check vg_field $vg vg_missing_pv_count 1
lvs -a -o+devices

# Check that MISSING flag is still set in ondisk metadata since the
# previously missing dev is still used by lv2.
pvck --dump metadata "$dev2" > meta
grep MISSING meta
rm meta

# The missing pv restrictions still apply since it has the MISSING flag.
not lvchange -ay $vg/$lv2
not lvcreate -l1 $vg

lvchange -ay --activationmode partial  $vg/$lv2

# Replace the missing leg of LV2 so no LV will be using the dev that was
# missing.  The MISSING_PV flag will not have been cleared from the
# metadata yet; that will take another metadata update.
lvconvert --repair --yes $vg/$lv2

lvs -a -o+devices | tee out
not grep "$dev1" out

# The MISSING_PV flag hasn't been cleared from the metadata yet, but now
# that the PV is not used by any more LVs, that flag will be cleared from
# the metadata in the next update.
pvck --dump metadata "$dev2" > meta
grep MISSING meta
rm meta

# Reporting commands run vg_read which sees MISSING_PV in the metadata,
# but vg_read then sees the dev is no longer used by any LV, so vg_read
# clears the MISSING_PV flag in the vg struct (not in the metadata) before
# returning the vg struct to the caller.  It's cleared in the vg struct so
# that the limitations of having a missing PV are not applied to the
# command.  The caller sees/uses/reports the VG as having no missing PV,
# even though the metadata still contains MISSING_PV.  The MISSING_PV flag
# is no longer needed in the metadata, but there has simply not been a
# metadata update yet to clear it.
# The message that's printed in this case is:
# WARNING: VG %s has unused reappeared PV %s %s
pvs -o+missing |tee out
not grep missing out
vgs -o+partial,missing_pv_count
check vg_field $vg vg_partial ""
check vg_field $vg vg_missing_pv_count 0

# Run any command that updates the metadata, and the MISSING_PV flag will
# be cleared.  Here just use lvcreate -l1, or we could use
# vgck --updatemetadata.
lvcreate -l1 $vg

# Now the MISSING flag is removed from the ondisk metadata.
pvck --dump metadata "$dev2" > meta
not grep MISSING meta
rm meta

# and commands continue to report no missing PV
pvs -o+missing |tee out
not grep missing out
vgs -o+partial,missing_pv_count
check vg_field $vg vg_partial ""
check vg_field $vg vg_missing_pv_count 0

vgchange -an $vg

vgremove -ff $vg
