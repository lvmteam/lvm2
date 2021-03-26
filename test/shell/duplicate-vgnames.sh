#!/usr/bin/env bash

# Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 7

# test setups:
#    # local vgs named foo  # foreign vg named foo
# a. 0                      1
# b. 0                      2
# c. 1                      1
# d. 1                      2
# e. 2                      0
# f. 2                      1
# g. 2                      2
# h. 3                      3
#
# commands to run for each test setup:
#
# vgs
# all cases show all local
#
# vgs --foreign
# all cases show all local and foreign
#
# vgs foo
# a. not found
# b. not found
# c. show 1 local
# d. show 1 local
# e-g. dup error
#
# vgs --foreign foo
# a. show 1 foreign
# b. dup error
# c. show 1 local
# d. show 1 local
# e-g. dup error
#
# vgchange -ay
# a. none
# b. none
# c. activate 1 local
# d. activate 1 local
# e-g. activate 2 local
# (if both local vgs have lvs with same name the second will fail to activate)
#
# vgchange -ay foo
# a. none
# b. none
# c. activate 1 local
# d. activate 1 local
# e-g. dup error
#
# lvcreate foo
# a. none
# b. none
# c. create 1 local
# d. create 1 local
# e-g. dup error
#
# vgremove foo
# a. none
# b. none
# c. remove 1 local
# d. remove 1 local
# e-g. dup error
# (in a couple cases test that vgremove -S vg_uuid=N works for local vg when local dups exist)


# a. 0 local, 1 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1

vgs -o+uuid |tee out
not grep $vg1 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out

not vgs -o+uuid $vg1 |tee out
not grep $vg1 out
vgs --foreign -o+uuid $vg1 |tee out
grep $vg1 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
not grep active out
vgchange -an

not vgchange -ay $vg1
lvs --foreign -o vguuid,active |tee out
not grep active out
vgchange -an

not lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | not grep $lv2

not vgremove $vg1
vgs --foreign -o+uuid |tee out
grep $UUID1 out
vgremove -y -S vg_uuid=$UUID1
vgs --foreign -o+uuid |tee out
grep $UUID1 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"

# b. 0 local, 2 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n $lv1 -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other2" $vg1
aux enable_dev "$dev1"

vgs -o+uuid |tee out
not grep $vg1 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out

not vgs -o+uuid $vg1 |tee out
not grep $vg1 out
not vgs --foreign -o+uuid $vg1 |tee out
not grep $vg1 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
not grep active out
vgchange -an

not vgchange -ay $vg1
lvs --foreign -o vguuid,active |tee out
not grep active out
vgchange -an

not lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | not grep $lv2
grep $UUID2 out | not grep $lv2

not vgremove $vg1
vgs --foreign -o+uuid |tee out
grep $UUID1 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"

# c. 1 local, 1 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n $lv1 -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux enable_dev "$dev1"

vgs -o+uuid |tee out
cat out
grep $vg1 out
grep $UUID1 out
not grep $UUID2 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out

vgs -o+uuid $vg1 |tee out
grep $vg1 out
grep $UUID1 out
not grep $UUID2 out
vgs --foreign -o+uuid $vg1 |tee out
grep $vg1 out
grep $UUID1 out
not grep $UUID2 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | not grep active
vgchange -an

vgchange -ay $vg1
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | not grep active
vgchange -an

lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | grep $lv2
grep $UUID2 out | not grep $lv2

vgremove -y $vg1
vgs -o+uuid |tee out
not grep $UUID1 out
vgs --foreign -o+uuid |tee out
grep $UUID2 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"

# d. 1 local, 2 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
lvcreate -n $lv1 -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux disable_dev "$dev2"
vgcreate $vg1 "$dev3"
lvcreate -n $lv1 -l1 -an $vg1
UUID3=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other2" $vg1
aux enable_dev "$dev1"
aux enable_dev "$dev2"

vgs -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
not grep $UUID2 out
not grep $UUID3 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out

vgs -o+uuid $vg1 |tee out
grep $vg1 out
grep $UUID1 out
not grep $UUID2 out
not grep $UUID3 out
vgs --foreign -o+uuid $vg1 |tee out
grep $vg1 out
grep $UUID1 out
not grep $UUID2 out
not grep $UUID3 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | not grep active
grep $UUID3 out | not grep active
vgchange -an

vgchange -ay $vg1
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | not grep active
grep $UUID3 out | not grep active
vgchange -an

lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | grep $lv2
grep $UUID2 out | not grep $lv2
grep $UUID3 out | not grep $lv2

vgremove -y $vg1
vgs -o+uuid |tee out
not grep $UUID1 out
vgs --foreign -o+uuid |tee out
grep $UUID2 out
grep $UUID3 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"
aux wipefs_a "$dev4"

# e. 2 local, 0 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
# diff lvname to prevent clash in vgchange -ay
lvcreate -n ${lv1}_b -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux enable_dev "$dev1"

vgs -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out

not vgs -o+uuid $vg1 |tee out
not grep $vg1 out
not vgs --foreign -o+uuid $vg1 |tee out
not grep $vg1 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | grep active
vgchange -an

not vgchange -ay $vg1
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | not grep active
grep $UUID2 out | not grep active
vgchange -an

not lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | not grep $lv2
grep $UUID2 out | not grep $lv2

not vgremove $vg1
vgs -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
vgremove -y -S vg_uuid=$UUID1
vgs -o+uuid |tee out
not grep $UUID1 out
grep $UUID2 out
vgremove -y -S vg_uuid=$UUID2
vgs -o+uuid |tee out
not grep $UUID1 out
not grep $UUID2 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"

# f. 2 local, 1 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
# diff lvname to prevent clash in vgchange -ay
lvcreate -n ${lv1}_b -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev2"
vgcreate $vg1 "$dev3"
lvcreate -n $lv1 -l1 -an $vg1
UUID3=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux enable_dev "$dev1"
aux enable_dev "$dev2"

vgs -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
not group $UUID3 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out

not vgs -o+uuid $vg1 |tee out
not grep $vg1 out
not vgs --foreign -o+uuid $vg1 |tee out
not grep $vg1 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | grep active
grep $UUID3 out | not grep active
vgchange -an

not vgchange -ay -vvvv $vg1
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | not grep active
grep $UUID2 out | not grep active
grep $UUID3 out | not grep active
vgchange -an

not lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | not grep $lv2
grep $UUID2 out | not grep $lv2
grep $UUID3 out | not grep $lv2

not vgremove $vg1
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
vgremove -y -S vg_uuid=$UUID1
vgs --foreign -o+uuid |tee out
not grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
vgremove -y -S vg_uuid=$UUID2
vgs --foreign -o+uuid |tee out
not grep $UUID1 out
not grep $UUID2 out
grep $UUID3 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"
aux wipefs_a "$dev4"

# g. 2 local, 2 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
# diff lvname to prevent clash in vgchange -ay
lvcreate -n ${lv1}_b -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev2"
vgcreate $vg1 "$dev3"
lvcreate -n $lv1 -l1 -an $vg1
UUID3=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux disable_dev "$dev3"
vgcreate $vg1 "$dev4"
lvcreate -n $lv1 -l1 -an $vg1
UUID4=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other2" $vg1
aux enable_dev "$dev1"
aux enable_dev "$dev2"
aux enable_dev "$dev3"

vgs -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
not group $UUID3 out
not group $UUID4 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
grep $UUID4 out

not vgs -o+uuid $vg1 |tee out
not grep $vg1 out
not vgs --foreign -o+uuid $vg1 |tee out
not grep $vg1 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | grep active
grep $UUID3 out | not grep active
grep $UUID4 out | not grep active
vgchange -an

not vgchange -ay $vg1
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | not grep active
grep $UUID2 out | not grep active
grep $UUID3 out | not grep active
grep $UUID4 out | not grep active
vgchange -an

not lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | not grep $lv2
grep $UUID2 out | not grep $lv2
grep $UUID3 out | not grep $lv2
grep $UUID4 out | not grep $lv2

not vgremove $vg1
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
grep $UUID4 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"
aux wipefs_a "$dev4"
aux wipefs_a "$dev5"

# h. 3 local, 3 foreign
# setup
vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -l1 -an $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev1"
vgcreate $vg1 "$dev2"
# diff lvname to prevent clash in vgchange -ay
lvcreate -n ${lv1}_b -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev2"
vgcreate $vg1 "$dev3"
# diff lvname to prevent clash in vgchange -ay
lvcreate -n ${lv1}_bb -l1 -an $vg1
UUID3=$(vgs --noheading -o vg_uuid $vg1 | xargs)
aux disable_dev "$dev3"
vgcreate $vg1 "$dev4"
lvcreate -n $lv1 -l1 -an $vg1
UUID4=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux disable_dev "$dev4"
vgcreate $vg1 "$dev5"
lvcreate -n $lv1 -l1 -an $vg1
UUID5=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other2" $vg1
aux disable_dev "$dev5"
vgcreate $vg1 "$dev6"
lvcreate -n $lv1 -l1 -an $vg1
UUID6=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other3" $vg1
aux enable_dev "$dev1"
aux enable_dev "$dev2"
aux enable_dev "$dev3"
aux enable_dev "$dev4"
aux enable_dev "$dev5"

vgs -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
not group $UUID4 out
not group $UUID5 out
not group $UUID6 out
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
grep $UUID4 out
grep $UUID5 out
grep $UUID6 out

not vgs -o+uuid $vg1 |tee out
not grep $vg1 out
not vgs --foreign -o+uuid $vg1 |tee out
not grep $vg1 out

vgchange -ay
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | grep active
grep $UUID2 out | grep active
grep $UUID3 out | grep active
grep $UUID4 out | not grep active
grep $UUID5 out | not grep active
grep $UUID6 out | not grep active
vgchange -an

not vgchange -ay $vg1
lvs --foreign -o vguuid,active |tee out
grep $UUID1 out | not grep active
grep $UUID2 out | not grep active
grep $UUID3 out | not grep active
grep $UUID4 out | not grep active
grep $UUID5 out | not grep active
grep $UUID6 out | not grep active
vgchange -an

not lvcreate -l1 -an -n $lv2 $vg1
lvs --foreign -o vguuid,name |tee out
grep $UUID1 out | not grep $lv2
grep $UUID2 out | not grep $lv2
grep $UUID3 out | not grep $lv2
grep $UUID4 out | not grep $lv2
grep $UUID5 out | not grep $lv2
grep $UUID6 out | not grep $lv2

not vgremove $vg1
vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
grep $UUID3 out
grep $UUID4 out
grep $UUID5 out
grep $UUID6 out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"
aux wipefs_a "$dev4"
aux wipefs_a "$dev5"
aux wipefs_a "$dev6"

# vgreduce test with 1 local and 1 foreign vg.
# setup
vgcreate $vg1 "$dev1" "$dev7"
lvcreate -n $lv1 -l1 -an $vg1 "$dev1"
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
PV1UUID=$(pvs --noheading -o uuid "$dev1")
PV7UUID=$(pvs --noheading -o uuid "$dev7")
aux disable_dev "$dev1"
aux disable_dev "$dev7"
vgcreate $vg1 "$dev2"
PV2UUID=$(pvs --noheading -o uuid "$dev2")
lvcreate -n $lv1 -l1 -an $vg1
UUID2=$(vgs --noheading -o vg_uuid $vg1 | xargs)
vgchange -y --systemid "other" $vg1
aux enable_dev "$dev1"
aux enable_dev "$dev7"

vgs --foreign -o+uuid |tee out
grep $vg1 out
grep $UUID1 out
grep $UUID2 out
pvs --foreign -o+uuid |tee out
grep $PV1UUID out
grep $PV7UUID out
grep $PV2UUID out

vgreduce $vg1 "$dev7"

pvs --foreign -o+uuid |tee out
grep $PV1UUID out
grep $PV7UUID out
grep $PV2UUID out

grep $PV7UUID out >out2
not grep $vg1 out2

vgremove -ff $vg1

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev7"
