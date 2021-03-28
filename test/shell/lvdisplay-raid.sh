#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#
# tests functionality lvdisplay tool for RAID
#

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_raid 1 7 0 || skip

aux prepare_vg 6

# raid0 loosing a leg
lvcreate -aey --type raid0 -i5 -l5 -n $lv $vg
lvdisplay $vg/$lv|grep "LV Status *available"
aux disable_dev "$dev1"
lvdisplay $vg/$lv|grep "LV Status *NOT available (partial)"
aux enable_dev "$dev1"
lvremove -y $vg/$lv

# raid1 loosing a leg/all legs
lvcreate -aey --type raid1 -m1 -l5 -n $lv $vg "$dev1" "$dev2"
lvdisplay $vg/$lv|grep "LV Status *available"
aux disable_dev "$dev1"
lvdisplay $vg/$lv|grep "LV Status *available (partial)"
aux disable_dev "$dev2"
lvdisplay $vg/$lv|grep "LV Status *NOT available (partial)"
aux enable_dev "$dev1" "$dev2"
lvremove -y $vg/$lv

# raid5 loosing a leg/2 legs
lvcreate -aey --type raid5 -i3 -l5 -n $lv $vg
lvdisplay $vg/$lv|grep "LV Status *available"
aux disable_dev "$dev1"
lvdisplay $vg/$lv|grep "LV Status *available (partial)"
aux disable_dev "$dev2"
lvdisplay $vg/$lv|grep "LV Status *NOT available (partial)"
aux enable_dev "$dev1" "$dev2"
lvremove -y $vg/$lv

# raid6 loosing a leg/2 legs/3 legs
lvcreate -aey --type raid6 -i3 -l5 -n $lv $vg
lvdisplay $vg/$lv|grep "LV Status *available"
aux disable_dev "$dev1"
lvdisplay $vg/$lv|grep "LV Status *available (partial)"
aux disable_dev "$dev2"
lvdisplay $vg/$lv|grep "LV Status *available (partial)"
aux disable_dev "$dev3"
lvdisplay $vg/$lv|grep "LV Status *NOT available (partial)"
aux enable_dev "$dev1" "$dev2" "$dev3"
lvremove -y $vg/$lv

# raid10 loosing a leg per mirror group / a complete mirror group
lvcreate -aey --type raid10 -i3 -l3 -n $lv $vg
lvdisplay $vg/$lv|grep "LV Status *available"
aux disable_dev "$dev1"
lvdisplay $vg/$lv|grep "LV Status *available (partial)"
aux disable_dev "$dev3"
lvdisplay $vg/$lv|grep "LV Status *available (partial)"
aux disable_dev "$dev6"
lvdisplay $vg/$lv|grep "LV Status *available (partial)"
aux enable_dev "$dev1" "$dev3" "$dev6"
lvdisplay $vg/$lv|grep "LV Status *available"
aux disable_dev "$dev1" "$dev2"
lvdisplay $vg/$lv|grep "LV Status *NOT available (partial)"
aux enable_dev "$dev1" "$dev2"

vgremove -y -f $vg
