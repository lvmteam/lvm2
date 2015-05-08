#!/bin/sh
# Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux prepare_pvs 5

vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev3" "$dev4" "$dev5"

aux disable_dev "$dev1"
pvscan
# dev1 is missing
fail pvs $(cat DEVICES)

vgcreate $vg1 "$dev2"
aux enable_dev "$dev1"

pvs "$dev1"

# reappearing device (rhbz 995440)
lvcreate -aey -m2 --type mirror -l4 --alloc anywhere --corelog -n $lv1 $vg2

aux disable_dev "$dev3"
lvconvert --yes --repair $vg2/$lv1
aux enable_dev "$dev3"

# here it should fix any reappeared devices
lvs $vg1 $vg2

lvs -a $vg2 -o+devices 2>&1 | tee out
not grep reappeared out

vgremove -ff $vg1 $vg2
