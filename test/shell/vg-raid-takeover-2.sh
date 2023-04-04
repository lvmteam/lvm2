#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Test VG takeover with raid LVs'

CONTINUE_ELSEWHERE=y
PREPARE_DEVS=4

. ./shell/vg-raid-takeover-1.sh

# fails because the missing dev is used by lvs
not vgreduce --removemissing $vg
# works because lvs can be used with missing leg
vgreduce --removemissing --mirrorsonly --force $vg

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

# unhide_dev before lvconvert --repair
# i.e. the device reappears before the LVs are repaired

aux unhide_dev "$dev1"

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

# this repairs lv1 by using dev3 in place of dev1
lvconvert -y --repair $vg/$lv1

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

# add a new disk to use for replacing dev1 in lv2
vgextend $vg "$dev4"

lvconvert -y --repair $vg/$lv2


test_check_mount


# let the new legs sync
aux wait_for_sync $vg $lv1
aux wait_for_sync $vg $lv2

vgck --updatemetadata $vg


test_check_mount


vgchange -an $vg
vgremove -f $vg
