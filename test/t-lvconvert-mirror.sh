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

. ./test-utils.sh
aux prepare_vg 5 80

# convert from linear to 2-way mirror
lvcreate -l2 -n $lv1 $vg $dev1
lvconvert -i1 -m+1 $vg/$lv1 $dev2 $dev3:0-1
check mirror $vg $lv1 $dev3
lvremove -ff $vg

# convert from 2-way mirror to linear
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvconvert -m-1 $vg/$lv1
check linear $vg $lv1
lvremove -ff $vg
# and now try removing a specific leg (bz453643)
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvconvert -m0 $vg/$lv1 $dev2
check lv_on $vg/$lv1 $dev1
lvremove -ff $vg

# convert from disklog to corelog, active
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvconvert -f --mirrorlog core $vg/$lv1
check mirror $vg $lv1 ""
lvremove -ff $vg

# convert from corelog to disklog, active
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2
lvconvert --mirrorlog disk $vg/$lv1 $dev3:0-1
check mirror $vg $lv1 $dev3
lvremove -ff $vg

# bz192865: lvconvert log of an inactive mirror lv
# convert from disklog to corelog, inactive
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvchange -an $vg/$lv1
echo y | lvconvert -f --mirrorlog core $vg/$lv1
check mirror $vg $lv1 ""
lvremove -ff $vg

# convert from corelog to disklog, inactive
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2
lvchange -an $vg/$lv1
lvconvert --mirrorlog disk $vg/$lv1 $dev3:0-1
check mirror $vg $lv1 $dev3
lvremove -ff $vg

# convert linear to 2-way mirror with 1 PV
lvcreate -l2 -n $lv1 $vg $dev1
not lvconvert -m+1 --mirrorlog core $vg/$lv1 $dev1
lvremove -ff $vg

lvcreate -l2 -m2 -n $lv1 $vg $dev1 $dev2 $dev4 $dev3:0-1
lvconvert -m-1 $vg/$lv1 $dev1
check mirror_images_on $lv1 $dev2 $dev4
lvconvert -m-1 $vg/$lv1 $dev2
check linear $vg $lv1
check lv_on $vg/$lv1 $dev4

