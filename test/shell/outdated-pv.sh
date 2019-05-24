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

#
# Test handling of "outdated PV" which occurs when a PV goes missing
# from a VG, and while it's missing the PV is removed from the VG.
# Then the PV reappears with the old VG metadata that shows it is a
# member.  That outdated metadata needs to be cleared.
#

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

lvcreate -n $lv1 -l1 -an $vg "$dev1"
lvcreate -n $lv2 -l1 -an $vg "$dev1"

aux disable_dev "$dev2"

vgreduce --removemissing $vg

pvs

aux enable_dev "$dev2"

pvs 2>&1 | tee out
grep "outdated" out

not pvs "$dev2"

# The VG can still be used with the outdated PV around
lvcreate -n $lv3 -l1 $vg
lvchange -ay $vg
lvs $vg
lvchange -an $vg

# Clears the outdated PV
vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "outdated" out

# The PV is no longer in the VG
pvs "$dev2" | tee out
not grep "$vg" out

# The cleared PV can be added back to the VG
vgextend $vg "$dev2"

pvs "$dev2" | tee out
grep "$vg" out

vgremove -ff $vg

