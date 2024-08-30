#!/usr/bin/env bash

# Copyright (C) 2010-2014 Red Hat, Inc. All rights reserved.
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

vgcreate $SHARED --metadatasize 128k $vg1 "$dev1"
lvcreate -l100%FREE -n $lv1 $vg1

# Test plain vgexport vgimport tools

# Argument is needed
invalid vgexport
invalid vgimport
# Cannot combine -a and VG name
invalid vgexport -a $vg
invalid vgimport -a $vg1
# Cannot export unknown VG
fail vgexport ${vg1}-non
fail vgimport ${vg1}-non
# Cannot export VG with active volumes
fail vgexport $vg1

vgchange -an $vg1
vgexport $vg1
# Already exported
fail vgexport $vg1

vgimport $vg1
# Already imported
fail vgimport $vg1
vgchange -ay $vg1

# Since the devices file is using devnames as ids,
# it will not automatically know that dev2 is a
# duplicate after the dd, so we need to remove dev2
# from df, then add it again after the dd.
if lvmdevices; then
	lvmdevices --deldev "$dev2"
fi

# Clone the LUN
dd if="$dev1" of="$dev2" bs=256K count=1

# Requires -y to confirm prompt about adding
# a duplicate pvid.
if lvmdevices; then
	lvmdevices -y --adddev "$dev2"
fi

# Verify pvs works on each device to give us vgname
aux hide_dev "$dev2"
check pv_field "$dev1" vg_name $vg1
aux unhide_dev "$dev2"

aux hide_dev "$dev1"
check pv_field "$dev2" vg_name $vg1
aux unhide_dev "$dev1"

lvmdevices || true
pvs -a -o+uuid

# Import the cloned PV to a new VG
vgimportclone --basevgname $vg2 "$dev2"

lvmdevices || true
pvs -a -o+uuid
vgs

# Verify we can activate / deactivate the LV from both VGs
lvchange -ay $vg1/$lv1 $vg2/$lv1
vgchange -an $vg1 $vg2

vgremove -ff $vg1 $vg2

pvremove "$dev1"
pvremove "$dev2"

# Test vgimportclone with incomplete list of devs, and with nomda PV.
vgcreate $SHARED --vgmetadatacopies 2 $vg1 "$dev1" "$dev2" "$dev3"
lvcreate -l1 -an $vg1
not vgimportclone -n newvgname "$dev1"
not vgimportclone -n newvgname "$dev2"
not vgimportclone -n newvgname "$dev3"
not vgimportclone -n newvgname "$dev1" "$dev2"
not vgimportclone -n newvgname "$dev1" "$dev3"
not vgimportclone -n newvgname "$dev2" "$dev3"
vgimportclone -n ${vg1}new "$dev1" "$dev2" "$dev3"
lvs ${vg1}new
vgremove -y ${vg1}new
pvremove "$dev1"
pvremove "$dev2"
pvremove "$dev3"

# Test importing a non-duplicate pv using the existing vg name
vgcreate $vg1 "$dev1"
vgimportclone -n $vg1 "$dev1"
vgs ${vg1}1
not vgs $vg1
vgremove ${vg1}1

# Test importing a non-duplicate pv using the existing vg name
# Another existing VG is using the initial generated vgname with
# the "1" suffix, so "2" is used.
vgcreate $vg1 "$dev1"
vgcreate ${vg1}1 "$dev2"
vgimportclone -n $vg1 "$dev1"
vgs ${vg1}1
vgs ${vg1}2
vgremove ${vg1}1
vgremove ${vg1}2
pvremove "$dev1"
pvremove "$dev2"

# Verify that if we provide the -n|--basevgname,
# the number suffix is not added unnecessarily.
vgcreate $SHARED --metadatasize 128k A${vg1}B "$dev1"

# vg1B is not the same as Avg1B - we don't need number suffix
dd if="$dev1" of="$dev2" bs=256K count=1
vgimportclone -n ${vg1}B "$dev2"
check pv_field "$dev2" vg_name ${vg1}B

# Avg1 is not the same as Avg1B - we don't need number suffix
dd if="$dev1" of="$dev2" bs=256K count=1
vgimportclone -n A${vg1} "$dev2"
check pv_field "$dev2" vg_name A${vg1}

# Avg1B is the same as Avg1B - we need to add the number suffix
dd if="$dev1" of="$dev2" bs=256K count=1
vgimportclone -n A${vg1}B "$dev2"
aux vgs
check pv_field "$dev2" vg_name A${vg1}B1

vgremove -ff A${vg1}B A${vg1}B1
