#!/usr/bin/env bash

# Copyright (C) 2019 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test vgsplit command options with cached volumes

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 3 0 || skip

aux prepare_vg 7
vgcfgbackup -f vgb $vg

lvcreate -L5 -n $lv2 $vg "$dev2"
lvcreate -L5 -n $lv3 $vg "$dev3"
lvconvert -y --type cache-pool --poolmetadata $vg/$lv2 $vg/$lv3

# Cannot split data and metadata from cache-pool
fail vgsplit $vg $vg1 "$dev2" 2>&1 | tee err
grep "Cannot split cache pool data" err

fail vgsplit $vg $vg1 "$dev3" 2>&1 | tee err
grep "Cannot split cache pool data" err

# Cache $lv1
lvcreate -L1 -n $lv1 $vg "$dev1"
lvconvert -y --cache --cachepool $vg/$lv3  $vg/$lv1



# Cannot move active cache
fail vgsplit $vg $vg1 "$dev1" "$dev2" "$dev3" 2>&1 | tee err
grep "must be inactive" err

vgchange -an $vg


# Try splitting component into separate VG
fail vgsplit $vg $vg1 "$dev1" 2>&1 | tee err
grep "Cannot split cache origin" err

fail vgsplit $vg $vg1 "$dev2" 2>&1 | tee err
grep "Cannot split cache origin" err

fail vgsplit $vg $vg1 "$dev3" 2>&1 | tee err
grep "Cannot split cache origin" err

fail vgsplit $vg $vg1 "$dev1" "$dev2" 2>&1 | tee err
grep "Cannot split cache origin" err

fail vgsplit $vg $vg1 "$dev2" "$dev3" 2>&1 | tee err

# Finally something that should pass
vgsplit $vg $vg1 "$dev1" "$dev2" "$dev3"

vgs $vg $vg1

test 4 -eq "$(get vg_field $vg pv_count)"
test 3 -eq "$(get vg_field $vg1 pv_count)"

lvremove -y $vg

# dm-cache with cachevol must not separated main LV and cachevol

vgremove -ff $vg
vgremove -ff $vg1

#
# Check we handle pmspare for split VGs
#
vgcfgrestore -f vgb $vg

# Create cache-pool and pmspare on single PV1
lvcreate -L10 --type cache-pool $vg/cpool "$dev1"
# Move spare to separate PV3
pvmove -n $vg/lvol0_pmspare "$dev1" "$dev3"
# Create origin on PV2
lvcreate -L10 -n orig $vg  "$dev2"
lvconvert -H -y --cachepool $vg/cpool $vg/orig

vgchange -an $vg

# Check we do not create new _pmspare
vgsplit --poolmetadataspare n  $vg $vg1 "$dev2" "$dev1"

check lv_exists $vg/lvol0_pmspare
check lv_not_exists $vg1/lvol0_pmspare

vgremove $vg
vgremove -f $vg1


vgcfgrestore -f vgb $vg

# Again - now with handling _pmspare by vgsplit
lvcreate -L10 --type cache-pool $vg/cpool "$dev1"
# Move spare to separate PV3
pvmove -n $vg/lvol0_pmspare "$dev1" "$dev3"
# Create origin on PV2
lvcreate -L10 -n orig $vg  "$dev2"
lvconvert -H -y --cachepool $vg/cpool $vg/orig

vgchange -an $vg

# Handle _pmspare  (default)
vgsplit --poolmetadataspare y  $vg $vg1 "$dev2" "$dev1"

check lv_not_exists $vg/lvol0_pmspare
check lv_exists $vg1/lvol0_pmspare

vgremove $vg
vgremove -f $vg1


vgcreate $vg "$dev1" "$dev2" "$dev3" "$dev4"

lvcreate -L6 -n $lv1 -an $vg "$dev2"
lvcreate -L6 -n $lv2 -an $vg "$dev3"
lvconvert -y --type cache --cachevol $lv2 $vg/$lv1
fail vgsplit $vg $vg1 "$dev2"
fail vgsplit $vg $vg1 "$dev3"
lvremove $vg/$lv1

vgremove -ff $vg
