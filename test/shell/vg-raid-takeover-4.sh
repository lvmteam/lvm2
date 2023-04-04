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

# unhide_dev
# the device reappears before the LVs are repaired
# and before the missing dev is removed from the vg

aux unhide_dev "$dev1"

pvs -o+missing
vgs -o+systemid,partial $vg
lvs -a -o+devices $vg

vgextend --restoremissing $vg "$dev1"


test_check_mount


vgchange -an $vg
vgremove -f $vg
