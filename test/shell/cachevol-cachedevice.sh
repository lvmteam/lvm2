#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 10 0 || skip
aux have_writecache 1 0 0 || skip

aux prepare_devs 4 64

vgcreate $SHARED $vg "$dev1" "$dev2"

## cache

# use existing cachevol
lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvcreate --type cache -n $lv2 -L40M --cachevol $lv1 -y $vg "$dev2"
check lv_field $vg/$lv2 segtype cache
check lv_field $vg/${lv1}_cvol segtype linear -a
lvremove -y $vg/$lv2

# use entire cachedevice for cachevol
lvcreate --type cache -n $lv2 -L40M --cachedevice "$dev1" -y $vg "$dev2"
check lv_field $vg/$lv2 segtype cache
check lv_field $vg/${lv2}_cache_cvol segtype linear -a
lvremove -y $vg/$lv2

# use part of cachedevice for cachevol
lvcreate --type cache -n $lv2 -L20M --cachedevice "$dev1" --cachesize 16M -y $vg "$dev2"
check lv_field $vg/$lv2 segtype cache
check lv_field $vg/${lv2}_cache_cvol segtype linear -a
lvcreate --type cache -n $lv3 -L20M --cachedevice "$dev1" --cachesize 16M -y $vg "$dev2"
check lv_field $vg/$lv3 segtype cache
check lv_field $vg/${lv3}_cache_cvol segtype linear -a
lvremove -y $vg/$lv2
lvremove -y $vg/$lv3

## writecache

# use existing cachevol
lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvcreate --type writecache -n $lv2 -L40M --cachevol $lv1 -y $vg "$dev2"
check lv_field $vg/$lv2 segtype writecache
check lv_field $vg/${lv1}_cvol segtype linear -a
lvremove -y $vg/$lv2

# use entire cachedevice for cachevol
lvcreate --type writecache -n $lv2 -L40M --cachedevice "$dev1" -y $vg "$dev2"
check lv_field $vg/$lv2 segtype writecache
check lv_field $vg/${lv2}_cache_cvol segtype linear -a
lvremove -y $vg/$lv2

# use part of cachedevice for cachevol
lvcreate --type writecache -n $lv2 -L20M --cachedevice "$dev1" --cachesize 16M -y $vg "$dev2"
check lv_field $vg/$lv2 segtype writecache
check lv_field $vg/${lv2}_cache_cvol segtype linear -a
lvcreate --type writecache -n $lv3 -L20M --cachedevice "$dev1" --cachesize 16M -y $vg "$dev2"
check lv_field $vg/$lv3 segtype writecache
check lv_field $vg/${lv3}_cache_cvol segtype linear -a
lvremove -y $vg/$lv2
lvremove -y $vg/$lv3

## multiple cachedevs

vgextend $vg "$dev3" "$dev4"

lvcreate --type writecache -n $lv2 -L100M --cachedevice "$dev1" --cachedevice "$dev3" -y $vg "$dev2" "$dev4"
check lv_field $vg/${lv2}_cache_cvol lv_size "120.00m"
lvremove -y $vg/$lv2

lvcreate --type writecache -n $lv2 -L100M --cachedevice "$dev1" --cachedevice "$dev3" --cachesize 80M -y $vg "$dev2" "$dev4"
check lv_field $vg/${lv2}_cache_cvol lv_size "80.00m"
lvremove -y $vg/$lv2

pvchange --addtag slow "$dev2"
pvchange --addtag slow "$dev4"
pvchange --addtag fast "$dev1"
pvchange --addtag fast "$dev3"

lvcreate --type writecache -n $lv2 -L100M --cachedevice @fast --cachesize 80M -y $vg @slow
check lv_field $vg/${lv2}_cache_cvol lv_size "80.00m"
lvremove -y $vg/$lv2

lvcreate --type cache -n $lv2 -L100M --cachedevice @fast --cachesize 80M -y $vg @slow
check lv_field $vg/${lv2}_cache_cvol lv_size "80.00m"
lvremove -y $vg/$lv2

## error cases

# cachevol doesn't exist
not lvcreate --type cache -n $lv2 -l8 --cachevol asdf -y $vg "$dev2"
not lvs $vg/$lv1
not lvs $vg/$lv2

# cachedevice doesn't exist
not lvcreate --type cache -n $lv2 -l8 --cachedevice asdf -y $vg "$dev2"
not lvs $vg/$lv1
not lvs $vg/$lv2

# cachevol doesn't exist
not lvcreate --type writecache -n $lv2 -l8 --cachevol asdf -y $vg "$dev2"
not lvs $vg/$lv1
not lvs $vg/$lv2

# cachedevice doesn't exist
not lvcreate --type writecache -n $lv2 -l8 --cachedevice asdf -y $vg "$dev2"
not lvs $vg/$lv1
not lvs $vg/$lv2

# when cachedevice is already being used, cachesize is required to use a part of it
lvcreate -n asdf -l1 $vg "$dev1"
not lvcreate --type writecache -n $lv2 -l8 --cachedevice "$dev1" -y $vg "$dev2"
not lvcreate --type writecache -n $lv2 -l8 --cachedevice "$dev1" --cachedevice "$dev3" -y $vg "$dev2"
not lvs $vg/$lv1
not lvs $vg/$lv2
lvcreate --type writecache -n $lv2 -l8 --cachedevice "$dev1" --cachesize 8M -y $vg "$dev2"
lvs $vg/$lv2
check lv_field $vg/${lv2}_cache_cvol lv_size "8.00m"
lvremove -y $vg/$lv2

vgremove -ff $vg

# lvconvert single step cachevol creation and attachment
# . cache and writecache
# . one or two cachedevices
# . with or without --cachesize
# . using tags for devices

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4"

lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvconvert -y --type cache --cachedevice "$dev2" $vg/$lv1
check lv_field $vg/$lv1 segtype cache
check lv_field $vg/${lv1}_cache_cvol segtype linear -a
check lv_field $vg/${lv1}_cache_cvol lv_size "60.00m"
lvs -o chunksize $vg/$lv1 |tee out
grep 64.00k out
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvconvert -y --type cache --cachedevice "$dev2" --chunksize 128k $vg/$lv1
check lv_field $vg/$lv1 segtype cache
check lv_field $vg/${lv1}_cache_cvol segtype linear -a
check lv_field $vg/${lv1}_cache_cvol lv_size "60.00m"
lvs -o chunksize $vg/$lv1 |tee out
grep 128.00k out
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvconvert -y --type cache --cachedevice "$dev2" --cachedevice "$dev3" $vg/$lv1
check lv_field $vg/$lv1 segtype cache
check lv_field $vg/${lv1}_cache_cvol lv_size "120.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvconvert -y --type cache --cachedevice "$dev2" --cachedevice "$dev3" --cachesize 8M $vg/$lv1
check lv_field $vg/$lv1 segtype cache
check lv_field $vg/${lv1}_cache_cvol lv_size "8.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvconvert -y --type writecache --cachedevice "$dev2" $vg/$lv1
check lv_field $vg/$lv1 segtype writecache
check lv_field $vg/${lv1}_cache_cvol lv_size "60.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvconvert -y --type writecache --cachedevice "$dev2" --cachedevice "$dev3" $vg/$lv1
check lv_field $vg/$lv1 segtype writecache
check lv_field $vg/${lv1}_cache_cvol lv_size "120.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

lvcreate -n $lv1 -l8 -an $vg "$dev1"
lvconvert -y --type writecache --cachedevice "$dev2" --cachedevice "$dev3" --cachesize 8M $vg/$lv1
check lv_field $vg/$lv1 segtype writecache
check lv_field $vg/${lv1}_cache_cvol lv_size "8.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

pvchange --addtag slow "$dev1"
pvchange --addtag fast "$dev2"
pvchange --addtag fast "$dev3"

lvcreate -n $lv1 -l8 -an $vg @slow
lvconvert -y --type cache --cachedevice @fast --cachesize 8M $vg/$lv1
check lv_field $vg/$lv1 segtype cache
check lv_field $vg/${lv1}_cache_cvol lv_size "8.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

lvcreate -n $lv1 -l8 -an $vg @slow
lvconvert -y --type writecache --cachedevice @fast --cachesize 8M $vg/$lv1
check lv_field $vg/$lv1 segtype writecache
check lv_field $vg/${lv1}_cache_cvol lv_size "8.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# if the cache name is used generate a new name
lvcreate -n $lv1 -l8 -an $vg @slow
lvcreate -n ${lv1}_cache -l1 -an $vg @slow
lvconvert -y --type writecache --cachedevice @fast --cachesize 8M $vg/$lv1
check lv_field $vg/$lv1 segtype writecache
check lv_field $vg/${lv1}_cache0_cvol lv_size "8.00m"
lvchange -ay $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

vgremove -ff $vg

