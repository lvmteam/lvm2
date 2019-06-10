#!/usr/bin/env bash

# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
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

lvcreate -an -Zn --type mirror -m 1 -l 1 -n mirror $vg

# try orphaning a missing PV (bz45867)
aux disable_dev "$dev1"
vgreduce --removemissing --force $vg
aux enable_dev "$dev1"

vgscan 2>&1 | tee vgscan.out
grep "Inconsistent metadata found for VG $vg" vgscan.out

# erase outdated dev1
vgck --updatemetadata $vg

vgscan 2>&1 | tee vgscan.out
not grep "Inconsistent metadata found for VG $vg" vgscan.out


vgextend $vg "$dev1"

lvcreate -l 1 -n boo -a n --zero n $vg

aux disable_dev "$dev1"

lvremove $vg/mirror

aux enable_dev "$dev1"

vgscan 2>&1 | tee vgscan.out
grep "Inconsistent metadata found for VG $vg" vgscan.out

# write the vg to update the metadata on dev1
vgck --updatemetadata $vg

vgscan 2>&1 | tee vgscan.out
not grep "Inconsistent metadata found for VG $vg" vgscan.out

aux disable_dev "$dev1"

vgreduce --removemissing --force $vg

aux enable_dev "$dev1"

vgscan 2>&1 | tee out

vgscan 2>&1 | tee vgscan.out
grep "Inconsistent metadata found for VG $vg" vgscan.out

# erase outdated dev1
vgck --updatemetadata $vg

vgscan 2>&1 | tee vgscan.out
not grep "Inconsistent metadata found for VG $vg" vgscan.out

vgremove -ff $vg
