#!/usr/bin/env bash

# Copyright (C) 2017-2020 Red Hat, Inc. All rights reserved.
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
which mkfs.xfs || skip

aux prepare_devs 6 70 # want 64M of usable space from each dev

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"

# lv1 is thinpool LV: 128M
# lv2 is fast LV:      64M
# lv3 is thin LV:       1G

#
# Test lvremove of a thinpool that uses cache|writecache on data
#

# attach writecache to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -n $lv2 -L64M -an $vg "$dev3"
lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1
lvchange -ay $vg/$lv1
lvs -a $vg
mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv3"
lvremove -y $vg/$lv1

# attach cache/writeback (cachevol) to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -n $lv2 -L64M -an $vg "$dev3"
lvconvert -y --type cache --cachevol $lv2 --cachemode writeback $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv3"
lvremove -y $vg/$lv1

# attach cache/writethrough (cachevol) to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -n $lv2 -L64M -an $vg "$dev3"
lvconvert -y --type cache --cachevol $lv2 --cachemode writethrough $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv3"
lvremove -y $vg/$lv1

# attach cache (cachepool) to thinpool data
lvcreate --type thin-pool -n $lv1 -L128M --poolmetadataspare n $vg "$dev1" "$dev2"
lvcreate --type thin -n $lv3 -V1G --thinpool $lv1 $vg
lvcreate -y --type cache-pool -n $lv2 -L64M --poolmetadataspare n $vg "$dev3" "$dev6"
lvconvert -y --type cache --cachepool $lv2 --poolmetadataspare n $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv3"
lvremove -y $vg/$lv1

vgremove -f $vg

