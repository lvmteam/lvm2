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
# Test "old metadata" repair which occurs when the VG is written
# and one of the PVs in the VG does not get written to, and then
# the PV reappears with the old metadata.  This can happen if
# a command is killed or crashes after writing new metadata to
# only some of the PVs in the VG, or if a PV is temporarily
# inaccessible while a VG is written.
#

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

#
# Test that vgck --updatemetadata will update old metadata.
#

lvcreate -n $lv1 -l1 -an $vg "$dev1"
lvcreate -n $lv2 -l1 -an $vg "$dev1"

aux disable_dev "$dev2"

pvs
pvs "$dev1"
not pvs "$dev2"
pvs "$dev3"
lvs $vg/$lv1
lvs $vg/$lv2

lvremove $vg/$lv2

aux enable_dev "$dev2"

pvs 2>&1 | tee out
grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

# fixes the old metadata on dev1
vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

#
# Test that any writing command will also update the
# old metadata.
#

lvcreate -n $lv2 -l1 -an $vg "$dev1"

aux disable_dev "$dev2"

pvs
pvs "$dev1"
not pvs "$dev2"
pvs "$dev3"
lvs $vg/$lv1
lvs $vg/$lv2

lvremove $vg/$lv2

aux enable_dev "$dev2"

pvs 2>&1 | tee out
grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

# fixes the old metadata on dev1
lvcreate -n $lv3 -l1 -an $vg

pvs 2>&1 | tee out
not grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

vgremove -ff $vg

#
# First two PVs with one mda, where both have old metadata.
# Third PV with two mdas, where the first mda has old
# metadata, and the second mda has current metadata.
#

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

pvcreate "$dev1"
pvcreate "$dev2"
pvcreate --pvmetadatacopies 2 "$dev3"

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

lvcreate -n $lv1 -l1 -an $vg "$dev3"
lvcreate -n $lv2 -l1 -an $vg "$dev3"

# Save the metadata at this point...
dd if="$dev1" of=meta1 bs=4k count=4
dd if="$dev2" of=meta2 bs=4k count=4
dd if="$dev3" of=meta3 bs=4k count=4

# and now change metadata so the saved copies are old
lvcreate -n $lv3 -l1 -an $vg "$dev3"

# Copy the saved metadata back to the three
# devs first mda, leaving the second mda on
# dev3 as the only latest copy of the metadata.

dd if=meta1 of="$dev1"
dd if=meta2 of="$dev2"
dd if=meta3 of="$dev3"

pvs 2>&1 | tee out
grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# We still see the three LVs since we are using
# the latest copy of metadata from dev3:mda2

lvs $vg/$lv1
lvs $vg/$lv2
lvs $vg/$lv3

# This command which writes the VG should update
# all of the old copies.
lvcreate -n $lv4 -l1 -an $vg

pvs 2>&1 | tee out
not grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
lvs $vg/$lv2
lvs $vg/$lv3
lvs $vg/$lv4

vgchange -an $vg
vgremove -ff $vg
