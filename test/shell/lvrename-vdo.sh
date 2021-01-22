#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Check online renaming of VDO devices works


SKIP_WITH_LVMPOLLD=1

. lib/inittest

#
# Main
#

aux have_vdo 6 2 1 || skip
aux have_cache 1 3 0 || skip
aux have_raid 1 3 0 || skip

aux prepare_vg 2 5000

lvcreate --vdo -L4G -V2G --name $lv1 $vg/vpool1
lvrename $vg/vpool1 vpool2
check lv_exists $vg $lv1 vpool2 vpool2_vdata

lvremove -ff $vg

# With version >= 6.2.3 online rename should work
if aux have_vdo 6 2 3 ; then

### CACHE ####
lvcreate --vdo -L4G -V2G --name $lv1 $vg/vpool1
lvcreate -H -L10 $vg/vpool1
lvrename $vg/vpool1 $vg/vpool2
check lv_exists $vg vpool2 vpool2_vdata
lvremove -ff $vg

### RAID ####
lvcreate --type raid1 -L4G --nosync --name vpool1 $vg
lvconvert --yes --type vdo-pool $vg/vpool1
lvrename $vg/vpool1 $vg/vpool2
check lv_exists $vg vpool2 vpool2_vdata vpool2_vdata_rimage_0
lvremove -ff $vg

fi # >= 6.2.3

# Check when VDO target does not support online resize
aux lvmconf "global/vdo_disabled_features = [ \"online_rename\" ]"


### CACHE ####
lvcreate --vdo -L4G -V2G --name $lv1 $vg/vpool1
lvcreate -H -L10 $vg/vpool1

# VDO target driver cannot handle online rename
not lvrename $vg/vpool1 $vg/vpool2 2>&1 | tee out
grep "Cannot rename" out

# Ofline should work
lvchange -an $vg
lvrename $vg/vpool1 $vg/vpool2
lvchange -ay $vg
check lv_exists $vg $lv1 vpool2 vpool2_vdata
lvremove -ff $vg


### RAID ####
lvcreate --type raid1 -L4G --nosync --name vpool1 $vg
lvconvert --yes --type vdo-pool $vg/vpool1
not lvrename $vg/vpool1 $vg/vpool2 2>&1 | tee out
grep "Cannot rename" out

# Ofline should work
lvchange -an $vg
lvrename $vg/vpool1 $vg/vpool2
lvchange -ay $vg
check lv_exists $vg vpool2 vpool2_vdata vpool2_vdata_rimage_0
lvremove -ff $vg

vgremove -ff $vg
