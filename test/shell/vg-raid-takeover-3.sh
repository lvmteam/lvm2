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

. ./shell/vg-raid-takeover-1.sh

#-----------------------
# replaces dev1 with dev3
lvconvert -y --repair $vg/$lv1

# no other disk to replace dev1 so remove the leg,
# but that's not allowed until the missing disk is removed from the vg
not lvconvert -y -m-1 $vg/$lv2
vgreduce --removemissing --mirrorsonly --force $vg

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg
lvconvert -y -m-1 $vg/$lv2


test_check_mount


aux unhide_dev "$dev1"

# put dev1 back into lv2,
# requires clearing outdated metadata and putting dev1 back in vg
vgck --updatemetadata $vg
pvs -o+missing
vgextend $vg "$dev1"
pvs -o+missing
lvconvert -y -m+1 $vg/$lv2 "$dev1"


test_check_mount


vgchange -an $vg
vgremove -f $vg
