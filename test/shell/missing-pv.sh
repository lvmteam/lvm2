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

. lib/inittest

aux prepare_devs 3
get_devs

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

lvcreate -n $lv1 -L8M --type mirror -m 1 $vg
lvcreate -n $lv2 -L8M --type mirror -m 1 $vg

vgchange -an $vg

pvs
vgs
lvs -a -o+devices

# Fail one leg of each mirror LV.
aux disable_dev "$dev1"

pvs
vgs
lvs -a -o+devices

# Cannot do normal activate of either LV with a failed leg.
not lvchange -ay $vg/$lv1
not lvchange -ay $vg/$lv2

# Can activate with partial option.
lvchange -ay --activationmode partial $vg/$lv1
lvchange -ay --activationmode partial $vg/$lv2

pvs
vgs
lvs -a -o+devices

# Repair lv1 so it no longer uses failed dev.
lvconvert --repair --yes $vg/$lv1

# Check that MISSING flag is set in ondisk metadata,
# it should have been written by the lvconvert since the
# missing PV is still used by lv2.
pvck --dump metadata "$dev2" > meta
grep MISSING meta
rm meta

pvs
vgs
lvs -a -o+devices

# Verify normal activation is possible of lv1 since it's
# not using any failed devs, and partial activation is
# required for lv2 since it's still using the failed dev.
vgchange -an $vg
lvchange -ay $vg/$lv1
not lvchange -ay $vg/$lv2
vgchange -an $vg

aux enable_dev "$dev1"

pvs
vgs
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

pvs
vgs
lvs -a -o+devices

# Check that MISSING flag is still set in ondisk
# metadata since the previously missing dev is still
# used by lv2.
pvck --dump metadata "$dev2" > meta
grep MISSING meta
rm meta


# The missing pv restrictions still apply since it
# has the MISSING flag.
not lvchange -ay $vg/$lv2
not lvcreate -l1 $vg

lvchange -ay --activationmode partial  $vg/$lv2

# After repair, no more LVs will be using the previously
# missing PV.
lvconvert --repair --yes $vg/$lv2

pvs
vgs
lvs -a -o+devices

vgchange -an $vg

# The next write of the metadata will clear the MISSING
# flag in ondisk metadata because the previously missing
# PV is no longer used by any LVs.

# Run a command to write ondisk metadata, which should clear
# the MISSING flag, could also use vgck --updatemetadata vg.
lvcreate -l1 $vg

# Check that the MISSING flag is no longer set
# in the ondisk metadata.
pvck --dump metadata "$dev2" > meta
not grep MISSING meta
rm meta


pvs
vgs
lvs -a -o+devices

vgchange -an $vg
vgremove -ff $vg

