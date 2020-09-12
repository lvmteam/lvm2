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

aux prepare_pvs 1 16777216
get_devs

vgcreate $SHARED -s 4K "$vg" "${DEVICES[@]}"

not lvcreate -T -L15.995T --poolmetadatasize 5G $vg/pool

lvs -ao+seg_pe_ranges $vg

vgremove -ff $vg
