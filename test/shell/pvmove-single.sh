#!/usr/bin/env bash

# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description="ensure pvmove works on single PV"
SKIP_WITH_LVMLOCKD=1

. lib/inittest

aux prepare_vg 1

lvcreate -aey -l16 -n $lv1 $vg
lvcreate -aey -l16 -n $lv2 $vg

lvs -ao+seg_pe_ranges $vg

pvmove -i0 --alloc anywhere  "$dev1:12-19" "$dev1"

lvs -ao+seg_pe_ranges,seg_size_pe $vg

# both LVs  should have now 2 segments  12extents + 4extents
lvs --noheadings -oseg_size_pe $vg > out
test 2 -eq "$(grep 12 out | wc -l)"
test 2 -eq "$(grep 4 out | wc -l)"

lvremove -ff $vg
