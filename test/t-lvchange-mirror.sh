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
aux prepare_vg 3

# force resync 2-way active mirror
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
check mirror $vg $lv1 $dev3
echo y | lvchange --resync $vg/$lv1
check mirror $vg $lv1 $dev3
lvremove -ff $vg

# force resync 2-way inactive mirror
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
lvchange -an $vg/$lv1
check mirror $vg $lv1 $dev3
lvchange --resync $vg/$lv1
check mirror $vg $lv1 $dev3
lvremove -ff $vg
