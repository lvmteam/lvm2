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

. lib/inittest

aux have_cache 1 3 0 || skip

aux prepare_vg 5 80

# lvcreate origin, lvcreate cache-pool, and lvconvert to cache
lvcreate -l 2 -n $lv1 $vg
lvcreate --type cache-pool -l 1 -n ${lv1}_cachepool $vg
lvconvert --type cache --cachepool $vg/${lv1}_cachepool $vg/$lv1
dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel
lvremove -ff $vg

# Bug 1095843
# lvcreate RAID1 origin, lvcreate cache-pool, and lvconvert to cache
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
lvcreate --type cache-pool -l 1 -n ${lv1}_cachepool $vg
lvconvert --type cache --cachepool $vg/${lv1}_cachepool $vg/$lv1
lvs -a $vg/${lv1}_corig_rimage_0        # ensure images are properly renamed
dmsetup table ${vg}-$lv1 | grep cache   # ensure it is loaded in kernel
lvremove -ff $vg

# lvcreate RAID1 origin, lvcreate RAID1 cache-pool, and lvconvert to cache
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
lvcreate --type raid1 -m 1 -l 2 -n ${lv1}_cachepool $vg
lvconvert --type cache-pool --yes $vg/${lv1}_cachepool
#should lvs -a $vg/${lv1}_cdata_rimage_0  # ensure images are properly renamed
lvconvert --type cache --cachepool $vg/${lv1}_cachepool $vg/$lv1
lvs -a $vg/${lv1}_corig_rimage_0        # ensure images are properly renamed
dmsetup table ${vg}-$lv1 | grep cache   # ensure it is loaded in kernel
lvremove -ff $vg

vgremove -f $vg
