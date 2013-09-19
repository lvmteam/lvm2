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

test_description="ensure pvmove works with thin segment types"

. lib/test
test -e LOCAL_CLVMD && skip
which mkfs.ext2 || skip
which md5sum || skip

aux target_at_least dm-thin-pool 1 8 0 || skip
# for stacking
aux target_at_least dm-raid 1 3 5 || skip

aux prepare_pvs 5 20
vgcreate -c n -s 128k $vg $(cat DEVICES)

# Each of the following tests does:
# 1) Create two LVs - one linear and one other segment type
#    The two LVs will share a PV.
# 2) Move both LVs together
# 3) Move only the second LV by name


# Testing pvmove of thin LV
lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -T $vg/${lv1}_pool -l 4 -V 8 -n $lv1 "$dev1"
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1"
aux mkdev_md5sum $vg $lv1
pvmove "$dev1" "$dev5"
check lv_tree_on $vg ${lv1}_foo "$dev5"
check lv_tree_on $vg $lv1 "$dev5"
check dev_md5sum $vg $lv1
pvmove -n $lv1 "$dev5" "$dev4"
check lv_tree_on $vg $lv1 "$dev4"
check lv_tree_on $vg ${lv1}_foo "$dev5"
check dev_md5sum $vg $lv1
lvremove -ff $vg

# Testing pvmove of thin LV on RAID
lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate --type raid1 -m 1 -l 4 -n ${lv1}_raid1_pool $vg "$dev1" "$dev2"
lvcreate --type raid1 -m 1 -L 2 -n ${lv1}_raid1_meta $vg "$dev1" "$dev2"
lvconvert --thinpool $vg/${lv1}_raid1_pool \
        --poolmetadata ${lv1}_raid1_meta
lvcreate -T $vg/${lv1}_raid1_pool -V 8 -n $lv1
check lv_tree_on $vg ${lv1}_foo "$dev1"
check lv_tree_on $vg $lv1 "$dev1" "$dev2"
aux mkdev_md5sum $vg $lv1
pvmove "$dev1" "$dev5"
check lv_tree_on $vg ${lv1}_foo "$dev5"
check lv_tree_on $vg $lv1 "$dev2" "$dev5"
check dev_md5sum $vg $lv1
pvmove -n $lv1 "$dev5" "$dev4"
check lv_tree_on $vg $lv1 "$dev2" "$dev4"
check lv_tree_on $vg ${lv1}_foo "$dev5"
check dev_md5sum $vg $lv1
lvremove -ff $vg
