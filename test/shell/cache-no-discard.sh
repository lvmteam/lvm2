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

# Check reporting of no_discard_passdown

SKIP_WITH_LVMPOLLD=1

# Until new version of cache_check tools - no integrity validation
LVM_TEST_CACHE_CHECK_CMD=""

. lib/inittest

aux kernel_at_least 5 1 || skip

aux have_cache 2 0

aux prepare_vg 1

# Create thinLV without discard
lvcreate --discards=ignore -T -V20 -L20 -n $lv $vg/pool

aux extend_filter_LVMTEST

# Use discard-less LV as PV for $vg1
pvcreate "$DM_DEV_DIR/$vg/$lv"
vgcreate -s 128K $vg1 "$DM_DEV_DIR/$vg/$lv"

# Create simple cache LV
lvcreate -aey -L2 -n $lv1 $vg1
lvcreate -H -L2 $vg1/$lv1

#lvs -ao+kernel_discards $vg1
check lv_field $vg1/$lv1 kernel_discards "nopassdown"

vgremove -f $vg1
vgremove -f $vg
