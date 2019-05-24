#!/usr/bin/env bash

# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
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

aux prepare_vg 3 12
get_devs

lvcreate -aye --type mirror -m 1 -l 1 -n mirror $vg
lvcreate -l 1 -n resized $vg
lvchange -a n $vg/mirror

aux backup_dev "${DEVICES[@]}"

makeold() {
	# reset metadata on all devs to starting condition
	aux restore_dev "${DEVICES[@]}"
	not check lv_field $vg/resized lv_size "8.00m"
	# change the metadata on all devs
	lvresize -L 8192K $vg/resized
	# reset metadata on just dev1 to the previous version
	aux restore_dev "$dev1"
}

# create old metadata
makeold

# reports old metadata
vgs $vg 2>&1 | tee cmd.out
grep "ignoring metadata" cmd.out
check lv_field $vg/resized lv_size "8.00m"

# corrects old metadata
lvcreate -l1 -an $vg

# no old report
vgs $vg 2>&1 | tee cmd.out
not grep "ignoring metadata" cmd.out
check lv_field $vg/resized lv_size "8.00m"


echo Check auto-repair of failed vgextend
echo - metadata written to original pv but not new pv

vgremove -f $vg
pvremove -ff "${DEVICES[@]}"
pvcreate "${DEVICES[@]}"

aux backup_dev "$dev2"
vgcreate $SHARED $vg "$dev1"
vgextend $vg "$dev2"
aux restore_dev "$dev2"

vgs -o+vg_mda_count $vg
pvs -o+vg_mda_count

should check compare_fields vgs $vg vg_mda_count pvs "$dev2" vg_mda_count

vgremove -ff $vg
