#!/usr/bin/env bash

# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# regression test for lvmetad reporting error:
# Internal error: LV snap_with_lvm1_meta (00000000000000000000000000000001) missing from preload metadata

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 2
get_devs

vgcreate --metadatatype 1 "$vg" "${DEVICES[@]}"

# Make origin volume
lvcreate -ae -l5 $vg -n origin

# Create a snap of origin
lvcreate -s $vg/origin -n snap_with_lvm1_meta -l4

# Remove volume snapper/snap_with_lvm1_meta
lvremove -f $vg/snap_with_lvm1_meta

vgremove -ff $vg
