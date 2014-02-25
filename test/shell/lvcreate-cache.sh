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

. lib/test

aux target_at_least dm-cache 1 3 0 || skip

# Skip in cluster for now, but should test EX mode...
test -e LOCAL_CLVMD && skip

aux prepare_vg 5 80

####################
# Cache_Pool creation
####################

# Full CLI (the advertised form)
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvremove -ff $vg/${lv}_cache_pool

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

# Create/remove cache_pool
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvremove -ff $vg

# Create cache_pool, then origin with cache, then remove all
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvcreate --type cache -l 2 $vg/${lv}_cache_pool -n $lv1
dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel
lvremove -ff $vg

# Create cache_pool, then origin with cache, then remove cache_pool/cache
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvcreate --type cache -l 2 $vg/${lv}_cache_pool -n $lv1
lvremove -ff $vg/${lv}_cache_pool
lvremove -ff $vg/$lv1

# Create cache_pool, then origin with cache, then remove origin
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvcreate --type cache -l 2 $vg/${lv}_cache_pool -n $lv1
lvremove -ff $vg/$lv1
lvremove -ff $vg/${lv}_cache_pool

# Shorthand CLI (cache_pool exists, create origin w/ cache)
#lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
#lvcreate --cache -l 2 $vg/${lv}_cache_pool -n $lv1
#lvremove -ff $vg

# Shorthand CLI (cache_pool exists, create origin w/ cache)
#lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
#lvcreate -H -l 2 $vg/${lv}_cache_pool -n $lv1
#lvremove -ff $vg

# Create origin, then cache_pool and cache
lvcreate -l 2 -n $lv1 $vg
lvcreate --type cache -l 1 $vg/$lv1
dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel
lvremove -ff $vg

# Shorthand CLI (origin exists, create cache_pool and cache)
#lvcreate -l 1 -n $lv1 $vg
#lvcreate --cache -l 2 $vg/$lv1
#lvremove -ff $vg

# Shorthand CLI (origin exists, create cache_pool and cache)
#lvcreate -l 1 -n $lv1 $vg
#lvcreate -H -l 2 $vg/$lv1
#lvremove -ff $vg


################################################
# Repeat key tests with 'writethrough' cachemode
################################################
# Create/remove cache_pool
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg --cachemode writethrough
lvremove -ff $vg

# Create cache_pool, then origin with cache, then remove all
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvcreate --type cache -l 2 $vg/${lv}_cache_pool -n $lv1 --cachemode writethrough
lvremove -ff $vg

# Create cache_pool, then origin with cache, then remove cache_pool/cache
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvcreate --type cache -l 2 $vg/${lv}_cache_pool -n $lv1 --cachemode writethrough
lvremove -ff $vg/${lv}_cache_pool
lvremove -ff $vg/$lv1

# Create cache_pool, then origin with cache, then remove origin
lvcreate --type cache-pool -l 1 -n ${lv}_cache_pool $vg
lvcreate --type cache -l 2 $vg/${lv}_cache_pool -n $lv1 --cachemode writethrough
lvremove -ff $vg/$lv1
lvremove -ff $vg/${lv}_cache_pool

# Create origin, then cache_pool and cache
lvcreate -l 2 -n $lv1 $vg
lvcreate --type cache -l 1 $vg/$lv1 --cachemode writethrough
lvremove -ff $vg


##############################
# Test things that should fail
##############################

# Attempt to create smaller cache than origin should fail
lvcreate -l 1 -n $lv1 $vg
not lvcreate --type cache -l 2 $vg/$lv1
lvremove -ff $vg


# Option testing
# --chunksize
# --cachepolicy
# --poolmetadatasize
# --poolmetadataspare
