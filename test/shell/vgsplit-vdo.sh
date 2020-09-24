#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test vgsplit command options with vdo volumes

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_vdo 6 2 0 || skip

aux lvmconf "allocation/vdo_slab_size_mb = 128"

aux prepare_vg 4 2200

lvcreate --vdo -L4G -n $lv1 $vg "$dev1" "$dev2"
lvcreate --vdo -L4G -n $lv2 $vg "$dev3" "$dev4"

# Cannot move only part of VDO _vdata
not vgsplit $vg $vg2 "$dev3" |& tee out
grep "split" out

# Cannot move active VDO
not vgsplit $vg $vg2 "$dev3" "$dev4" |& tee out
grep "inactive" out

lvchange -an $vg/$lv2

vgsplit $vg $vg2 "$dev3" "$dev4"

lvchange -ay $vg2/$lv2
lvs -ao+devices $vg $vg2

# Cannot merge active VDO
not vgmerge $vg $vg2 |& tee out
grep "inactive" out

lvchange -an $vg2/$lv2

vgmerge $vg $vg2

lvs -ao+devices $vg

lvchange -ay $vg/$lv2

vgremove -ff $vg
