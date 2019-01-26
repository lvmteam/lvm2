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

aux have_vdo 6 2 0 || skip
aux have_cache 1 3 0 || skip

aux prepare_vg 1 9000

lvcreate -L10 -n cpool $vg
lvcreate -L4G -n vpool $vg

# Cache volume
lvconvert --yes --cache --cachepool cpool $vg/vpool

# Stack cached LV as  VDODataLV for VDOPoolLV
lvconvert --yes --type vdo-pool -V50M --name $lv1 $vg/vpool

aux mkdev_md5sum $vg $lv1

lvconvert --splitcache $vg/vpool

check dev_md5sum $vg $lv1
lvchange -an $vg
lvchange -ay $vg
check dev_md5sum $vg $lv1

lvconvert --yes --cache --cachepool cpool $vg/vpool
lvconvert --splitcache $vg/vpool

lvs -a $vg

lvconvert --yes --cache --cachepool cpool $vg/$lv1
check dev_md5sum $vg $lv1
lvchange -an $vg
lvchange -ay $vg
check dev_md5sum $vg $lv1

lvs -a  $vg
not lvconvert --splitcache $vg/vpool
lvconvert --splitcache $vg/$lv1
lvs -a  $vg

# Also check, removal of cached VDO LV works
lvconvert --yes --cache --cachepool cpool $vg/$lv1
vgremove -f $vg
