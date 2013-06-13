#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Test vgsplit command options for validity

. lib/test

aux have_thin 1 0 0 || skip

aux prepare_devs 5

vgcreate $vg1 $(cat DEVICES)
lvcreate -T -L8M $vg1/pool1 -V10M -n $lv1 "$dev1" "$dev2"
lvcreate -T -L8M $vg1/pool2 -V10M -n $lv2 "$dev3" "$dev4"

# Test with external origin if available
lvcreate -l1 -an -pr --zero n -n eorigin $vg1 "$dev5"
aux have_thin 1 5 0 && lvcreate -an -s $vg1/eorigin -n $lv3 --thinpool $vg1/pool1

# Cannot move active thin
not vgsplit $vg1 $vg2 "$dev1" "$dev2" "$dev5"

vgchange -an $vg1
not vgsplit $vg1 $vg2 "$dev1"
not vgsplit $vg1 $vg2 "$dev2" "$dev3"
vgsplit $vg1 $vg2 "$dev1" "$dev2" "$dev5"
lvs -a -o+devices $vg1 $vg2

vgmerge $vg1 $vg2

vgremove -ff $vg1
