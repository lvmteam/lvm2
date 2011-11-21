#!/bin/sh
# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test
aux prepare_vg 5 80

# extend 2-way mirror
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvchange -an $vg/$lv1
lvextend -l+2 $vg/$lv1
check mirror $vg $lv1 $dev3
check mirror_images_contiguous $vg $lv1
lvremove -ff $vg

# reduce 2-way mirror
lvcreate -l4 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvchange -an $vg/$lv1
lvreduce -l-2 $vg/$lv1
check mirror $vg $lv1 $dev3
lvremove -ff $vg

# extend 2-way mirror (cling if not contiguous)
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvcreate -l1 -n $lv2 $vg $dev1
lvcreate -l1 -n $lv3 $vg $dev2
lvchange -an $vg/$lv1
lvextend -l+2 $vg/$lv1
check mirror $vg $lv1 $dev3
check mirror_images_clung $vg $lv1
lvremove -ff $vg
