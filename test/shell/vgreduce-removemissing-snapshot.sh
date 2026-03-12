#!/usr/bin/env bash

# Copyright (C) 2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

aux prepare_vg 5

lvcreate --type mirror -m 3 -L 2M -n 4way $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5":0
lvcreate -s $vg/4way -L 2M -n snap
lvcreate -i 2 -L 2M $vg "$dev1" "$dev2" -n stripe

aux disable_dev "$dev2" "$dev4"
echo n | lvconvert --repair $vg/4way
aux enable_dev "$dev2" "$dev4"
#not vgreduce --removemissing $vg
vgreduce -v --removemissing --force $vg # "$dev2" "$dev4"
lvs -a -o +devices $vg | not grep unknown
lvs -a -o +devices $vg
check mirror $vg 4way "$dev5"

vgremove -ff $vg

# snapshot of mirror
vgcreate $vg "$dev1" "$dev2" "$dev3"
lvcreate --type mirror -m 1 -L 2M -n mirror_lv $vg
lvcreate -s $vg/mirror_lv -L 2m -n snap
aux disable_dev "$dev1"
vgreduce --removemissing --force $vg
lvs -a -o +devices $vg | not grep unknown
# both mirror_lv and snap should be removed
not lvs $vg/mirror_lv
not lvs $vg/snap
aux enable_dev "$dev1"
vgck --updatemetadata "$vg"
vgremove -ff $vg

if aux have_raid 1 0 0; then
# snapshot of raid
vgcreate $vg "$dev1" "$dev2" "$dev3"
lvcreate --type raid1 -m 1 -L 2M -n raid_lv $vg
lvcreate -s $vg/raid_lv -L 2m -n snap
aux disable_dev "$dev1"
vgreduce --removemissing --force $vg
lvs -a -o +devices $vg | not grep unknown
# both raid_lv and snap should be removed
not lvs $vg/raid_lv
not lvs $vg/snap
aux enable_dev "$dev1"
vgremove -ff $vg
fi
