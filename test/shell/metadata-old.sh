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

aux clear_devs "$dev1" "$dev2" "$dev3"

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

# Test when the metadata on two PVs have the same seqno
# but different checksums.

aux clear_devs "$dev1" "$dev2"

pvcreate "$dev1"
pvcreate "$dev2"

vgcreate $SHARED $vg "$dev1" "$dev2"

lvcreate -n $lv1 -l1 -an $vg

pvck --dump metadata -f meta "$dev2"

# change an unimportant character so the metadata is effectively
# the same in content but will have a different checksum
sed 's/Linux/linux/' meta > meta2

# write out the changed metadata
pvck --repair -y -f meta2 "$dev2"

# the vg can still be used but will produce warnings
# the mda on one pv is updated, but not the other,
# which changes the error from a checksum inconsistency
# into a seqno inconsistency.
lvs $vg 2>&1 | tee out
grep WARNING out
grep $lv1 out
lvcreate -n $lv2 -l1 -an $vg 2>&1 |tee out
grep WARNING out
lvs $vg 2>&1 | tee out
grep WARNING out
grep $lv1 out
grep $lv2 out

# correct the senqo inconsistency
vgck --updatemetadata $vg
lvs $vg 2>&1 | tee out
not grep WARNING out
grep $lv1 out
grep $lv2 out

vgremove -ff $vg

# Old metadata is reattached that references PVs
# which are now used by other VGs.  Commands run
# on the VG from the old metadata should not
# clobber the other PVs.

vgcreate $vg1 "$dev1" "$dev2" "$dev3"
aux disable_dev "$dev1"
vgreduce --removemissing $vg1
vgremove $vg1
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
aux enable_dev "$dev1"
pvs | tee out
grep "$dev1" out | tee out1
grep "$dev2" out | tee out2
grep "$dev3" out | tee out3
grep $vg1 out1
grep $vg2 out2
grep $vg3 out3

# The old VG cannot be used until the invalid PV references
# are removed from it.
not lvcreate -l1 $vg1
not vgextend --restoremissing $vg1 "$dev1"
not vgextend --restoremissing $vg1 "$dev2"
not vgmerge $vg1 $vg2
not vgrename $vg1 foo
not vgremove $vg1
not vgremove -ff $vg1
not vgreduce $vg1 "$dev2"
not vgreduce $vg1 "$dev3"
not vgreduce --removemissing $vg1
not vgreduce --removemissing --force $vg1
not vgchange -ay $vg1

# To modify the old VG on the reattached device,
# removemissing together with --devices so the
# command only sees the old device.
vgreduce --removemissing --devices "$dev1" $vg1
pvs | tee out
grep "$dev1" out | tee out1
grep "$dev2" out | tee out2
grep "$dev3" out | tee out3
grep $vg1 out1
grep $vg2 out2
grep $vg3 out3

vgremove $vg1
pvs | tee out
not grep $vg1 out
grep "$dev2" out | tee out2
grep "$dev3" out | tee out3
grep $vg2 out2
grep $vg3 out3
vgremove $vg2
vgremove $vg3

# Repeat the previous, but with two reattached/old PVs
# referencing one other PV that's been reused for something
# else.

vgcreate $vg1 "$dev1" "$dev2" "$dev3"
aux disable_dev "$dev1"
aux disable_dev "$dev2"
vgreduce --removemissing $vg1
vgremove $vg1
vgcreate $vg3 "$dev3"
aux enable_dev "$dev1"
aux enable_dev "$dev2"
pvs | tee out
grep "$dev1" out | tee out1
grep "$dev2" out | tee out2
grep "$dev3" out | tee out3
grep $vg1 out1
grep $vg1 out2
grep $vg3 out3

# The old VG cannot be used until the invalid PV references
# are removed from it.
not lvcreate -l1 $vg1
not vgextend --restoremissing $vg1 "$dev3"
not vgmerge $vg1 $vg3
not vgrename $vg1 foo
not vgremove $vg1
not vgremove -ff $vg1
not vgreduce $vg1 "$dev2"
not vgreduce $vg1 "$dev3"
not vgreduce --removemissing $vg1
not vgreduce --removemissing --force $vg1
not vgchange -ay $vg1

# To modify the old VG on the reattached device,
# removemissing together with the correct PVs
# specified with --devices.
vgreduce --removemissing --devices "$dev1","$dev2" $vg1
pvs | tee out
grep "$dev1" out | tee out1
grep "$dev2" out | tee out2
grep "$dev3" out | tee out3
grep $vg1 out1
grep $vg1 out2
grep $vg3 out3

vgremove $vg1
pvs | tee out
not grep $vg1 out
grep "$dev3" out | tee out3
grep $vg3 out3
vgremove $vg3

