#!/usr/bin/env bash

# Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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

aux prepare_devs 3
get_devs

pvcreate --metadatacopies 0 "${DEVICES[@]}"
not vgcreate $SHARED "$vg" "${DEVICES[@]}"

aux lvmconf "metadata/dirs = [ \"$TESTDIR/mda\" ]"

vgcreate $SHARED $vg "$dev1"
check vg_field $vg vg_mda_count 1
vgremove -ff $vg

vgcreate $SHARED "$vg" "${DEVICES[@]}"
check vg_field $vg vg_mda_count 1
vgremove -ff $vg

pvcreate --metadatacopies 1 --metadataignore y "$dev1"
vgcreate $SHARED "$vg" "${DEVICES[@]}"
check vg_field $vg vg_mda_count 2
vgremove -ff $vg

pvcreate --metadatacopies 1 --metadataignore n "$dev1"
vgcreate $SHARED "$vg" "${DEVICES[@]}"
check vg_field $vg vg_mda_count 2
vgremove -ff $vg

pvcreate --metadatacopies 0 "$dev1"
aux lvmconf "metadata/dirs = [ \"$TESTDIR/mda\", \"$TESTDIR/mda2\" ]"
vgcreate $SHARED "$vg" "${DEVICES[@]}"
check vg_field $vg vg_mda_count 2
vgremove -ff $vg
