#!/bin/sh
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Demonstrate problem when upconverting and cutting leg in clvmd

. lib/inittest

aux prepare_pvs 3

vgcreate -s 64k $vg $(cat DEVICES)

lvcreate -aey -l10 --type mirror -m1 -n $lv1 $vg "$dev1" "$dev2"

# Slow down device so we are able to start next conversion in parallel
aux delay_dev "$dev3" 0 200

lvconvert -m+1 -b $vg/$lv1 "$dev3"

# To fix - wait helps here....
#lvconvert $vg/$lv1

lvs -a $vg

#
# It fails so use 'should' and -vvvv for now
#
should lvconvert -vvvv -m-1 $vg/$lv1 "$dev2"

vgremove -f $vg
