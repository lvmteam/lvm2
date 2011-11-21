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
aux lvmconf 'allocation/maximise_cling = 0'
aux lvmconf 'allocation/mirror_logs_require_separate_pvs = 1'

# 2-way mirror with corelog, 2 PVs
lvcreate -l2 -m1 --mirrorlog core -n $lv1 $vg $dev1 $dev2
check mirror_images_redundant $vg $lv1
lvremove -ff $vg

# 2-way mirror with disklog, 3 PVs
lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2 $dev3:0-1
check mirror_images_redundant $vg $lv1
check mirror_log_on $vg $lv1 $dev3
lvremove -ff $vg

# 3-way mirror with disklog, 4 PVs
lvcreate -l2 -m2 --mirrorlog disk -n $lv1 $vg $dev1 $dev2 $dev4 $dev3:0-1
check mirror_images_redundant $vg $lv1
check mirror_log_on $vg $lv1 $dev3
lvremove -ff $vg

# lvcreate --nosync is in 100% sync after creation (bz429342)
lvcreate -l2 -m1 --nosync -n $lv1 $vg $dev1 $dev2 $dev3:0-1 2>out
grep "New mirror won't be synchronised." out
lvs -o copy_percent --noheadings $vg/$lv1 | grep 100.00
lvremove -ff $vg

# creating 2-way mirror with disklog from 2 PVs fails
not lvcreate -l2 -m1 -n $lv1 $vg $dev1 $dev2
