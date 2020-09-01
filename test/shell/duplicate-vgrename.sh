#!/usr/bin/env bash

# Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 4

# a. 0 local, 1 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1

not vgrename $vg1 $vg2
vgs --foreign -o+uuid |tee out
grep $UUID1 out
not vgrename $UUID1 $vg2
vgs --foreign -o+uuid |tee out
grep $UUID1 out

lvs --foreign

aux wipefs_a "$dev1"

# b. 0 local, 2 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other2" $vg1
aux enable_dev "$dev1"

not vgrename $vg1 $vg2
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
not grep $vg2 out
grep $UUID1 out
grep $UUID2 out
not vgrename $UUID1 $vg2
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
not grep $vg2 out
grep $UUID1 out
grep $UUID2 out

lvs --foreign

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"

# c. 1 local, 1 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux enable_dev "$dev1"

vgrename $vg1 $vg2
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out
not vgrename $vg2 $vg1
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out

lvs --foreign

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"

# d. 1 local, 2 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux disable_dev "$dev2"
vgcreate $vg1 "$dev3"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID3=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other2" $vg1
aux enable_dev "$dev1"
aux enable_dev "$dev2"

vgrename $vg1 $vg2
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
not vgrename $vg2 $vg1
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out

lvs --foreign

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"

# e. 2 local, 0 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n ${lv1}_b -l1 -ky -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux enable_dev "$dev1"

not vgrename $vg1 $vg2
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
not grep $vg2 out
grep $UUID1 out
grep $UUID2 out
vgrename $UUID1 $vg2
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out
not vgrename $UUID2 $vg2
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out

lvs --foreign

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"

# f. 2 local, 1 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n ${lv1}_b -l1 -ky -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev2"
vgcreate $vg1 "$dev3"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID3=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux enable_dev "$dev1"
aux enable_dev "$dev2"
lvs --foreign

not vgrename $vg1 $vg2
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
not grep $vg2 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
vgrename $UUID1 $vg2
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
vgrename $vg1 $vg3
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $vg3 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
not vgrename $vg2 $vg1
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $vg3 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
not vgrename $vg2 $vg3
vgs --foreign -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $vg3 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out

lvs --foreign

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"

# g. 3 local, 0 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -ky -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n ${lv1}_b -l1 -ky -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev2"
vgcreate $vg1 "$dev3"
lvcreate -n ${lv1}_c -l1 -ky -an $vg1
UUID3=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux enable_dev "$dev1"
aux enable_dev "$dev2"

not vgrename $vg1 $vg2
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
not grep $vg2 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
vgrename $UUID1 $vg2
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
not vgrename $vg1 $vg2
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
not vgrename $vg1 $vg3
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
not grep $vg3 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
not vgrename $UUID2 $vg2
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
not grep $vg3 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
vgrename $UUID2 $vg3
vgs -o+uuid |tee out
lvs --foreign
grep $vg1 out
grep $vg2 out
grep $vg3 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out

lvs --foreign

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"

