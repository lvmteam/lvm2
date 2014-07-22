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

for mode in "" "--cachemode writethrough"
do

####################
# Cache_Pool creation
####################

# Full CLI (the advertised form)
lvcreate --type cache-pool -l 1 -n cache_pool $vg $mode
lvremove -f $vg/cache_pool

# Shorthand CLI (not advertised) -- not yet implemented
# lvcreate --cache -l 1 vg
# lvremove -ff $vg

# Shorthand CLI (not advertised) -- not yet implemented
# lvcreate -H -l 1 vg
# lvremove -ff $vg

################
# Cache creation
# Creating a cache is a two phase process
# - first, cache_pool (or origin)
# - then, the cache LV (lvcreate distinguishes supplied origin vs cache_pool)
################

# Create cache_pool, then origin with cache, then remove all
lvcreate --type cache-pool -l 1 -n cache_pool $vg
lvcreate --type cache -l 2 $vg/cache_pool -n $lv1 $mode
dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel
lvremove -f $vg

# Create cache_pool, then origin with cache, then remove cache_pool/cache
lvcreate --type cache-pool -l 1 -n cache_pool $vg
lvcreate --type cache -l 2 $vg/cache_pool -n $lv1 $mode
lvremove -f $vg/cache_pool
lvremove -f $vg/$lv1

# Create cache_pool, then origin with cache, then remove origin
lvcreate --type cache-pool -l 1 -n cache_pool $vg
lvcreate --type cache -l 2 $vg/cache_pool -n $lv1 $mode
lvremove -f $vg/$lv1
lvremove -f $vg/cache_pool

# Shorthand CLI (cache_pool exists, create origin w/ cache)
#lvcreate --type cache-pool -l 1 -n cache_pool $vg
#lvcreate --cache -l 2 $vg/cache_pool -n $lv1
#lvremove -f $vg

# Shorthand CLI (cache_pool exists, create origin w/ cache)
#lvcreate --type cache-pool -l 1 -n cache_pool $vg
#lvcreate -H -l 2 $vg/cache_pool -n $lv1
#lvremove -f $vg

# Bug 1110026
# Create origin, then cache_pool and cache
lvcreate -aey -l 2 -n $lv1 $vg
lvcreate --type cache -l 1 $vg/$lv1
#should dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel
lvremove -ff $vg

# Bug 1110026 & Bug 1095843
# Create RAID1 origin, then cache_pool and cache
lvcreate -aey -l 2 -n $lv1 $vg
lvcreate --type cache -l 1 $vg/$lv1
#should lvs -a $vg/${lv1}_corig_rimage_0        # ensure images are properly renamed
#should dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel
lvremove -ff $vg

# Shorthand CLI (origin exists, create cache_pool and cache)
#lvcreate -l 1 -n $lv1 $vg
#lvcreate --cache -l 2 $vg/$lv1
#lvremove -ff $vg

# Shorthand CLI (origin exists, create cache_pool and cache)
#lvcreate -l 1 -n $lv1 $vg
#lvcreate -H -l 2 $vg/$lv1
#lvremove -ff $vg

done

##############################
# Test things that should fail
##############################

# Attempt to create smaller cache than origin should fail
lvcreate -aey -l 1 -n $lv1 $vg
not lvcreate --type cache -l 2 $vg/$lv1


# Option testing
# --chunksize
# --cachepolicy
# --poolmetadatasize
# --poolmetadataspare

vgremove -ff $vg
