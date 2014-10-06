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

# Exercise creation of cache and cache pool volumes

# Full CLI uses  --type
# Shorthand CLI uses --cache | -H

. lib/inittest

aux have_cache 1 3 0 || skip

# FIXME: parallel cache metadata allocator is crashing when used value 8000!
aux prepare_vg 5 80000

#######################
# Cache_Pool creation #
#######################

# FIXME: Unsupported yet creation of cache pool and cached volume at once
# needs some policy to determine cache pool size
invalid lvcreate -H -l 1 $vg
invalid lvcreate --cache -l 1 $vg
invalid lvcreate --type cache -l 1 $vg

# Unlike in thin pool case - cache pool and cached volume both need size arg.
# So we require cache pool to exist and need to fail when it's missing.
#
# FIXME: introduce  --poolsize to make this command possible to pass
fail lvcreate -l 1 -H --cachepool $vg/pool3
fail lvcreate -l 1 -H --cachepool pool4 $vg
fail lvcreate -l 1 --type cache --cachepool $vg/pool5
fail lvcreate -l 1 --type cache --cachepool pool6 $vg
# --cachpool bring implicit --cache
fail lvcreate -l 1 --cachepool pool7 $vg

# Check nothing has been created yet
check vg_field $vg lv_count 0

# If the cache pool volume doesn't yet exist -> cache pool creation
lvcreate -l 1 -H $vg/pool1
lvcreate -l 1 --type cache $vg/pool2

# With cache-pool we are clear what has to be created
lvcreate -l 1 --type cache-pool $vg/pool3
lvcreate -l 1 --type cache-pool --cachepool $vg/pool4
lvcreate -l 1 --type cache-pool --cachepool pool5 $vg
lvcreate -l 1 --type cache-pool --name pool6 $vg
lvcreate -l 1 --type cache-pool --name $vg/pool7

check lv_field $vg/pool1 segtype "cache-pool"
check lv_field $vg/pool2 segtype "cache-pool"
check lv_field $vg/pool3 segtype "cache-pool"
check lv_field $vg/pool4 segtype "cache-pool"
check lv_field $vg/pool5 segtype "cache-pool"
check lv_field $vg/pool6 segtype "cache-pool"
check lv_field $vg/pool7 segtype "cache-pool"

lvremove -f $vg

# Validate ambiguous pool name is detected
invalid lvcreate -l 1 --type cache-pool --cachepool pool1 $vg/pool2
invalid lvcreate -l 1 --type cache-pool --name pool3 --cachepool pool4 $vg
invalid lvcreate -l 1 --type cache-pool --name pool6 --cachepool pool6 $vg/pool7
invalid lvcreate -l 1 --type cache-pool --name pool8 $vg/pool9
check vg_field $vg lv_count 0

for mode in "" "--cachemode writethrough"
do

################
# Cache creation
# Creating a cache is a two phase process
# - first, cache_pool (or origin)
# - then, the cache LV (lvcreate distinguishes supplied origin vs cache_pool)
################

lvcreate --type cache-pool -l 1 -n pool $vg $mode
# Select automatic name for cached LV
lvcreate --type cache -l1 $vg/pool

lvcreate --type cache-pool -l 1 -n pool1 $vg $mode
lvcreate --cache -l1 -n $lv1 --cachepool $vg/pool1
dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel

lvcreate --type cache-pool -l 1 -n pool2 $vg $mode
lvcreate -H -l1 -n $lv2 --cachepool pool2 $vg

#
# Now check removals
#

# Removal of cached LV removes every related LV
check lv_field $vg/$lv1 segtype "cache"
lvremove -f $vg/$lv1
check lv_not_exists $vg $lv1 pool1 pool1_cdata pool1_cmeta
# to preserve cachepool use  lvconvert --splitcache $vg/$lv1

# Removal of cache pool leaves origin uncached
check lv_field $vg/$lv2 segtype "cache"
lvremove -f $vg/pool2
check lv_not_exists $vg pool2 pool2_cdata pool2_cmeta
check lv_field $vg/$lv2 segtype "linear"

lvremove -f $vg

done

# Conversion through lvcreate case
# Bug 1110026
# Create origin, then cache pool and cache the origin
lvcreate -aey -l 2 -n $lv1 $vg
lvcreate --type cache -l 1 $vg/$lv1
dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel

# Bug 1110026 & Bug 1095843
# Create RAID1 origin, then cache pool and cache
lvcreate -aey -l 2 --type raid1 -m1 -n $lv2 $vg
lvcreate --cache -l 1 $vg/$lv2
check lv_exists $vg/${lv2}_corig_rimage_0	# ensure images are properly renamed
dmsetup table ${vg}-$lv2 | grep cache		# ensure it is loaded in kernel

lvremove -f $vg


# Check minimum cache pool metadata size
lvcreate -l 1 --type cache-pool --poolmetadatasize 1 $vg 2>out
grep "WARNING: Minimum" out

# FIXME: This test is failing in allocator with smaller VG sizes
lvcreate -l 1 --type cache-pool --poolmetadatasize 17G $vg 2>out
grep "WARNING: Maximum" out

lvremove -f $vg


##############################
# Test things that should fail
##############################

# Atempt to use bigger chunk size then cache pool data size
fail lvcreate -l 1 --type cache-pool --chunksize 16M $vg 2>out
grep "is bigger" out

# Option testing
# --chunksize
# --cachepolicy
# --poolmetadatasize
# --poolmetadataspare

lvremove -f $vg
lvcreate -n corigin -m 1 --type raid1 -l 10 $vg
lvcreate -n cpool --type cache $vg/corigin -l 10
check active $vg corigin_corig
dmsetup table | grep ^$PREFIX | grep corigin_corig

vgremove -ff $vg
