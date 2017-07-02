#!/usr/bin/env bash

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITHOUT_LVMETAD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 2
pvcreate --metadatatype 1 "$dev1"
pvs | tee out
grep "$dev1" out
vgcreate --metadatatype 1 $vg1 "$dev1"
vgs | tee out
grep $vg1 out
pvs | tee out
grep "$dev1" out

# check for RHBZ 1080189 -- SEGV in lvremove/vgremove
pvcreate -ff -y --metadatatype 1 "$dev1" "$dev2"
vgcreate --metadatatype 1 $vg1 "$dev1" "$dev2"
lvcreate -l1 $vg1 "$dev1"
pvremove -ff -y "$dev2"
vgchange -an $vg1
not lvremove $vg1
not vgremove -ff -y $vg1
