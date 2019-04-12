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

lvcreate -n $lv1 -L8M $vg "$dev2"
lvcreate -n $lv2 -L8M $vg "$dev3"
lvcreate -n $lv3 -L8M $vg "$dev2"
lvcreate -n $lv4 -L8M $vg "$dev3"

vgchange -an $vg

pvs
vgs
lvs -a -o+devices

# Fail device that is not used by any LVs.
aux disable_dev "$dev1"

pvs
vgs
lvs -a -o+devices

# Cannot do normal activation of LVs not using failed PV.
lvchange -ay $vg/$lv1
lvchange -ay $vg/$lv2

vgchange -an $vg

# Check that MISSING flag is not set in ondisk metadata.
pvck --dump metadata "$dev2" > meta
not grep MISSING meta
rm meta

pvs
vgs
lvs -a -o+devices

# lvremove is one of the few commands that is allowed to run
# when PVs are missing.  The vg_write from this command sets
# the MISSING flag on the PV in the ondisk metadata.
# (this could be changed, the MISSING flag wouldn't need
# to be set in the first place since the PV isn't used.)
lvremove $vg/$lv1

# Check that MISSING flag is set in ondisk metadata.
pvck --dump metadata "$dev2" > meta
grep MISSING meta
rm meta

# with MISSING flag in metadata, restrictions apply
not lvcreate -l1 $vg

aux enable_dev "$dev1"

# No LVs are using the PV with MISSING flag, so no restrictions
# are applied, and the vg_write here clears the MISSING flag on disk.
lvcreate -l1 $vg

# Check that MISSING flag is not set in ondisk metadata.
pvck --dump metadata "$dev2" > meta
not grep MISSING meta
rm meta


pvs
vgs
lvs -a -o+devices

vgchange -an $vg
vgremove -ff $vg

