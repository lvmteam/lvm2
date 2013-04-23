#!/bin/bash
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux target_at_least dm-raid 1 3 2 || skip

aux prepare_vg 4

# rhbz 889358
# Should be able to activate when RAID10
# has failed devs in different mirror sets.
lvcreate --type raid10 -m 1 -i 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvchange -an $vg/$lv1
aux disable_dev "$dev1" "$dev3"
lvchange -ay $vg/$lv1 --partial
lvchange -an $vg/$lv1

aux enable_dev "$dev1"
lvremove -ff $vg
