#!/usr/bin/env bash

# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Exercise usage of stacked cache volume used in thin pool volumes

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 3 0 || skip
aux have_thin 1 0 0 || skip

aux prepare_vg 5 80

lvcreate -L10 -n cpool $vg
lvcreate -L10 -n tpool $vg
lvcreate -L10 -n $lv1 $vg

lvconvert --yes --cache --cachepool cpool $vg/tpool

# Currently the only allowed stacking is cache thin data volume
lvconvert --yes --type thin-pool $vg/tpool

lvcreate -V10 -T -n $lv2 $vg/tpool

aux mkdev_md5sum $vg $lv2

lvconvert --splitcache $vg/tpool

check dev_md5sum $vg $lv2
lvchange -an $vg
lvchange -ay $vg
check dev_md5sum $vg $lv2

lvs -a $vg
lvconvert --yes --cache --cachepool cpool $vg/tpool

lvconvert --yes -T --thinpool $vg/tpool $vg/$lv1
check lv_field $vg/tpool segtype "thin-pool"
check lv_field $vg/$lv1 segtype "thin"
lvconvert --uncache $vg/tpool
lvs -a $vg

lvremove -f $vg

# Check conversion of cached LV works as thin-pool
lvcreate -L10 -n $lv $vg
lvcreate -L10 -n $lv1 $vg
lvcreate -H -L10 $vg/$lv

# Stack of cache over cache is unsupported ATM
fail lvconvert --yes --cachepool $vg/$lv

# Thin-pool cannot use cached metaddata LV  (meta should be on FAST device)
fail lvconvert --yes --thinpool $vg/$lv1 --poolmetadata $vg/$lv

# Thin-pool CAN use cached data LV
lvconvert --yes --thinpool $vg/$lv

vgremove -f $vg
