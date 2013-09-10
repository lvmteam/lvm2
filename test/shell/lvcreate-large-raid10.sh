#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# 'Exercise some lvcreate diagnostics'

. lib/test

aux target_at_least dm-raid 1 3 0 || skip

aux prepare_vg 5

lvcreate -s -l 20%FREE -n $lv1 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv2 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv3 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv4 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv5 $vg --virtualsize 256T

aux extend_filter_LVMTEST

pvcreate $DM_DEV_DIR/$vg/$lv[12345]
vgcreate $vg1 $DM_DEV_DIR/$vg/$lv[12345]

#
# Create large RAID LVs
#
# We need '--nosync' or our virtual devices won't work

lvcreate --type raid10 -m 1 -i 2 -L 200T -n $lv1 $vg1 --nosync
check lv_field $vg1/$lv1 size "200.00t"
lvremove -ff $vg1

lvremove -ff $vg
