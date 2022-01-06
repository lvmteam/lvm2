#!/usr/bin/env bash

# Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 2

vgcreate $vg1 "$dev1"
vgchange --setautoactivation n $vg1
UUID1=$(vgs --noheading -o vg_uuid $vg1 | xargs)
lvcreate -l1 -an -n $lv1 $vg1
dd if="$dev1" of="$dev2" bs=1M count=1
aux disable_dev "$dev1"
vgrename $vg1 $vg2
pvchange -u "$dev2"
aux enable_dev "$dev1"

vgs -o+uuid |tee out
grep $vg1 out | tee out1
grep $UUID1 out1
grep $vg2 out | tee out2
grep $UUID1 out2

vgs $vg1
vgs $vg2
lvs $vg1/$lv1
lvs $vg2/$lv1

lvremove $vg1/$lv1
lvremove $vg2/$lv1

lvcreate -l1 -an -n $lv2 $vg1
lvcreate -l1 -an -n $lv3 $vg2

vgchange -u $vg2

vgs -o uuid $vg1 |tee out
grep $UUID1 out

vgs -o uuid $vg2 |tee out
not grep $UUID1 out

vgremove -ff $vg1
vgremove -ff $vg2
