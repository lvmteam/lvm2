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

aux target_at_least dm-raid 1 1 0 || skip

aux prepare_vg 5

lvcreate -s -l 20%FREE -n $lv1 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv2 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv3 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv4 $vg --virtualsize 256T
lvcreate -s -l 20%FREE -n $lv5 $vg --virtualsize 256T

#FIXME this should be 1024T
#check lv_field $vg/$lv size "128.00m"

aux extend_filter_LVMTEST

pvcreate $DM_DEV_DIR/$vg/$lv[12345]
vgcreate $vg1 $DM_DEV_DIR/$vg/$lv[12345]

# bz837927 START

#
# Create large RAID LVs
#
# We need '--nosync' or our virtual devices won't work
lvcreate --type raid1 -m 1 -L 200T -n $lv1 $vg1 --nosync
check lv_field $vg1/$lv1 size "200.00t"
lvremove -ff $vg1

for segtype in raid4 raid5 raid6; do
        lvcreate --type $segtype -i 3 -L 750T -n $lv1 $vg1 --nosync
        check lv_field $vg1/$lv1 size "750.00t"
        lvremove -ff $vg1
done

#
# Convert large linear to RAID1 (belong in different test script?)
#
lvcreate -aey -L 200T -n $lv1 $vg1
# Need to deactivate or the up-convert will start sync'ing
lvchange -an $vg1/$lv1
lvconvert --type raid1 -m 1 $vg1/$lv1
check lv_field $vg1/$lv1 size "200.00t"
lvremove -ff $vg1

#
# Extending large RAID LV (belong in different script?)
#
lvcreate --type raid1 -m 1 -L 200T -n $lv1 $vg1 --nosync
check lv_field $vg1/$lv1 size "200.00t"
lvextend -L +200T $vg1/$lv1
check lv_field $vg1/$lv1 size "400.00t"
lvremove -ff $vg1

# bz837927 END

lvremove -ff $vg
