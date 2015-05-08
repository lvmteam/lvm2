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

# Exercise conversion of cache and cache pool

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux have_cache 1 3 0 || skip

aux prepare_vg 5 80

lvcreate --type cache-pool -an -v -L 2 -n cpool $vg
lvcreate -H -L 4 -n corigin --cachepool $vg/cpool

fail lvcreate -s -L2 $vg/corigin
fail lvcreate -s -L2 $vg/cpool
fail lvcreate -s -L2 $vg/cpool_cdata
fail lvcreate -s -L2 $vg/cpool_cmeta

###########################
# Check regular converion #
###########################
# lvcreate origin, lvcreate cache-pool, and lvconvert to cache
lvcreate -an -Zn -L 2 -n $lv1 $vg
lvcreate -L 8 -n $lv2 $vg
lvcreate -an -Zn -L 8 -n $lv3 $vg
lvcreate -an -Zn -L 8 -n $lv4 $vg
lvcreate -an -Zn -L 16 -n $lv5 $vg

lvconvert --yes --type cache-pool --cachepool $vg/$lv1
check inactive $vg ${lv1}_cdata
lvconvert --yes --type cache-pool --chunksize 256 $vg/$lv2
check inactive $vg ${lv2}_cdata
check lv_field $vg/$lv2 chunk_size "256.00k"

# Check swap of cache pool metadata
lvconvert --yes --type cache-pool --poolmetadata $lv4 $vg/$lv3
UUID=$(get lv_field $vg/$lv5 uuid)
lvconvert --yes --cachepool $vg/$lv3 --poolmetadata $lv5
check lv_field $vg/${lv3}_cmeta uuid "$UUID"


#fail lvconvert --cachepool $vg/$lv1 --poolmetadata $vg/$lv2
#lvconvert --yes --type cache-pool --poolmetadata $vg/$lv2 $vg/$lv1
#lvconvert --yes --poolmetadata $vg/$lv2 --cachepool $vg/$lv1

lvremove -ff $vg

lvcreate -L 2 -n $lv1 $vg
lvcreate --type cache-pool -l 1 -n ${lv1}_cachepool $vg
lvconvert --cache --cachepool $vg/${lv1}_cachepool $vg/$lv1
dmsetup table ${vg}-$lv1 | grep cache  # ensure it is loaded in kernel

#lvconvert --cachepool $vg/${lv1}_cachepool $vg/$lv1
#lvconvert --cachepool $vg/${lv1}_cachepool --poolmetadatasize 20 "$dev3"


fail lvconvert --type cache --cachepool $vg/${lv1}_cachepool $vg/$lv1

# Test --splitcache leaves both cache origin and cache pool
lvconvert --splitcache $vg/$lv1
check lv_exists $vg $lv1 ${lv1}_cachepool
lvremove -f $vg


lvcreate -n corigin -l 10 $vg
lvcreate -n pool -l 10 $vg
lvs -a -o +devices
fail lvconvert --type cache --cachepool $vg/pool $vg/corigin
lvconvert --yes --cache --cachepool $vg/pool $vg/corigin
lvremove -ff $vg


#######################
# Invalid conversions #
#######################
lvcreate -an -Zn -L 2 -n $lv1 $vg
lvcreate -an -Zn -L 8 -n $lv2 $vg
lvcreate -an -Zn -L 8 -n $lv3 $vg
lvcreate -an -Zn -L 8 -n $lv4 $vg

# Undefined cachepool
invalid lvconvert --type cache --poolmetadata $vg/$lv2 $vg/$lv1

# Cannot mix with thins
invalid lvconvert --type cache --poolmetadata $vg/$lv2 --thinpool $vg/$lv1
invalid lvconvert --type cache --thin --poolmetadata $vg/$lv2 $vg/$lv1

# Undefined cached volume
invalid lvconvert --type cache --cachepool $vg/$lv1
invalid lvconvert --cache --cachepool $vg/$lv1

# Single vg is required
invalid lvconvert --type cache --cachepool $vg/$lv1 --poolmetadata $vg1/$lv2 $vg/$lv3
invalid lvconvert --type cache --cachepool $vg/$lv1 --poolmetadata $lv2 $vg1/$lv3
invalid lvconvert --type cache --cachepool $vg1/$lv1 --poolmetadata $vg2/$lv2 $vg/$lv3
invalid lvconvert --type cache-pool --poolmetadata $vg2/$lv2 $vg1/$lv1

invalid lvconvert --cachepool $vg1/$lv1 --poolmetadata $vg2/$lv2

# Invalid syntax, vg is unknown
invalid lvconvert --yes --cachepool $lv3 --poolmetadata $lv4

# Invalid chunk size is <32KiB >1GiB
invalid lvconvert --type cache-pool --chunksize 16 --poolmetadata $lv2 $vg/$lv1
invalid lvconvert --type cache-pool --chunksize 2G --poolmetadata $lv2 $vg/$lv1

# Invalid chunk size is bigger then data size, needs to open VG
fail lvconvert --yes --type cache-pool --chunksize 16M --poolmetadata $lv2 $vg/$lv1

lvremove -f $vg

##########################
# Prohibited conversions #
##########################
lvcreate --type cache-pool -L10 $vg/$lv1
lvcreate --cache -L20 $vg/$lv1
lvcreate -L10 -n $lv2 $vg

fail lvconvert --yes --type cache $vg/$lv2 --cachepool $vg/$lv1
fail lvconvert --yes --type cache $vg/$lv1 --cachepool $vg/$lv2
fail lvconvert --yes --type cache-pool $vg/$lv1
fail lvconvert --yes --type mirror -m1 $vg/$lv1
not aux have_raid 1 0 0 || fail lvconvert --yes --type raid1 -m1 $vg/$lv1
fail lvconvert --yes --type snapshot $vg/$lv1 $vg/$lv2
fail lvconvert --yes --type snapshot $vg/$lv2 $vg/$lv1
not aux have_thin 1 0 0 || fail lvconvert --yes -T --thinpool $vg/$lv2 $vg/$lv1

lvremove -f $vg

vgremove -f $vg
