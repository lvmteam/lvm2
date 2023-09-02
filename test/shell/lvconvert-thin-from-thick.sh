#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test conversion to thin volume from thick LVs

SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

which mkfs.ext4 || skip
which fsck || skip
aux have_tool_at_least "$LVM_TEST_THIN_RESTORE_CMD" 0 3 1 || skip
aux have_thin 1 5 0 || skip

_convert_to_thin() {
	mkfs.ext4 -E nodiscard "$DM_DEV_DIR/$vg/$lv1"
	lvconvert --yes --type thin  $vg/$lv1
	fsck -n "$DM_DEV_DIR/$vg/$lv1"
	check lv_field $vg/$lv1 segtype thin
	lvs -ao+segtype $vg
	lvremove -f $vg
}


#
# Main
#
aux prepare_vg 2 6000

# error -> thin
lvcreate --type error -Zn -L10 -n $lv1 $vg
lvconvert --yes --type thin  $vg/$lv1 "$dev2"
check lv_on $vg ${lv1}_tpool0_tmeta "$dev2"
check lv_on $vg lvol0_pmspare "$dev2"
not dd if="$DM_DEV_DIR/$vg/$lv1" of=/dev/null bs=512 count=1
lvremove -f $vg

# zero -> thin
lvcreate --type zero -L2T -n $lv1 $vg
lvconvert --yes --type thin  $vg/$lv1 "$dev1"
check lv_on $vg ${lv1}_tpool0_tmeta "$dev1"
check lv_on $vg lvol0_pmspare "$dev1"
lvremove -f $vg

# zero -> thin --test
if [ ! -e LOCAL_LVMLOCKD ] ; then
# FIXME: missing support with lvmlockd
lvcreate --type zero -L2T -n $lv1 $vg
lvconvert --yes --type thin --test $vg/$lv1
check lv_field $vg/$lv1 segtype zero
check vg_field $vg lv_count 1
lvremove -f $vg
fi

# linear -> thin
lvcreate -L10 -n $lv1 $vg
_convert_to_thin

# raid1 -> thin
if aux have_raid 1 7 0 ; then
	lvcreate --type raid1 -L10 -n $lv1 $vg
	_convert_to_thin
fi

# cache -> thin
if aux have_cache 1 3 0 ; then
	lvcreate -L10 -n $lv1 $vg
	lvcreate -H -L10 $vg/$lv1
	_convert_to_thin
fi

# writecache -> thin
if aux have_writecache 1 0 0 ; then
	lvcreate -L10 -n $lv1 $vg
	lvcreate -an -L10 -n $lv2 $vg
	lvconvert --yes --type writecache --cachevol $lv2 $vg/$lv1
	_convert_to_thin
fi

# vdo -> thin
if aux have_vdo 6 2 0 ; then
	lvcreate --type vdo -L4G -n $lv1 $vg/$lv2
	_convert_to_thin
fi

vgremove -ff $vg
