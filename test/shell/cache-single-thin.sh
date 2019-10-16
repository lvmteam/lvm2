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

# Test single lv cache

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 10 0 || skip
aux have_thin 1 0 0 || skip

aux prepare_devs 5 80

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"

# lv1 starts as a standard linear LV
# lv1 is then sped up by attaching fast device lv2 using dm-cache
# lv1 is then used as the data device in a thin pool

lvcreate -L10 -an -n $lv1 $vg "$dev1"
lvcreate -L10 -an -n $lv2 $vg "$dev2"

lvconvert -y --type cache --cachevol $lv2 $vg/$lv1
lvconvert -y --type thin-pool $vg/$lv1

lvcreate --type thin -V10 -n lvthin --thinpool $vg/$lv1

lvchange -an $vg/lvthin
lvchange -an $vg/$lv1

# detach the cache (lv2) from lv1

lvconvert --splitcache $vg/$lv1

vgremove -ff $vg

