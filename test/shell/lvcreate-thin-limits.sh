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

# test allocation of thin-pool on limiting extents number

SKIP_WITH_LVMPOLLD=1

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

# FIXME  update test to make something useful on <16T
aux can_use_16T || skip

#
# Main
#
aux have_thin 1 0 0 || skip
which mkfs.ext4 || skip

# 16T device
aux prepare_pvs 2 8388608
get_devs

# gives 16777215M device
vgcreate $SHARED -s 4M "$vg" "${DEVICES[@]}"

# For 1st. pass only single PV
lvcreate -l100%PV --name $lv1 $vg "$dev2"

for i in 1 0
do
	SIZE=$(get vg_field "$vg" vg_free --units m)
	SIZE=${SIZE%%\.*}

	# ~16T - 2 * 5G + something  -> should not fit
	not lvcreate -Zn -T -L$(( SIZE - 2 * 5 * 1024 + 1 )) --poolmetadatasize 5G $vg/pool

	check vg_field "$vg" lv_count "$i"

	# Should fit  data + metadata + pmspare
	lvcreate -Zn -T -L$(( SIZE - 2 * 5 * 1024 )) --poolmetadatasize 5G $vg/pool

	check vg_field "$vg" vg_free "0"

	lvs -ao+seg_pe_ranges $vg

        # Remove everything for 2nd. pass
	lvremove -ff $vg
done

vgremove -ff $vg
