#!/bin/sh
# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Check pvmove --abort behaviour for all VGs and PVs

. lib/inittest

aux prepare_pvs 6 60

vgcreate -s 128k $vg "$dev1" "$dev2"
pvcreate --metadatacopies 0 "$dev3"
vgextend $vg "$dev3"
vgcreate -s 128k $vg1 "$dev4" "$dev5"
pvcreate --metadatacopies 0 "$dev6"
vgextend $vg1 "$dev6"

# Slowdown writes
aux delay_dev "$dev3" 0 800 $(get first_extent_sector "$dev3"):
aux delay_dev "$dev6" 0 800 $(get first_extent_sector "$dev6"):

for mode in "--atomic" "" ;
do
for backgroundarg in "-b" "" ;
do

# Create multisegment LV
lvcreate -an -Zn -l30 -n $lv1 $vg "$dev1"
lvcreate -an -Zn -l30 -n $lv2 $vg "$dev2"
lvcreate -an -Zn -l30 -n $lv1 $vg1 "$dev4"
lvextend -l+30 -n $vg1/$lv1 "$dev5"

cmd1=(pvmove -i1 $backgroundarg $mode "$dev1" "$dev3")
cmd2=(pvmove -i1 $backgroundarg $mode "$dev2" "$dev3")
cmd3=(pvmove -i1 $backgroundarg $mode -n $vg1/$lv1 "$dev4" "$dev6")

if test -z "$backgroundarg" ; then
	"${cmd1[@]}" &
	aux wait_pvmove_lv_ready "$vg-pvmove0"
	"${cmd2[@]}" &
	aux wait_pvmove_lv_ready "$vg-pvmove1"
	"${cmd3[@]}" &
	aux wait_pvmove_lv_ready "$vg1-pvmove0"
        lvs -a $vg $vg1
else
	LVM_TEST_TAG="kill_me_$PREFIX" "${cmd1[@]}"
	LVM_TEST_TAG="kill_me_$PREFIX" "${cmd2[@]}"
	LVM_TEST_TAG="kill_me_$PREFIX" "${cmd3[@]}"
fi

# test removal of all pvmove LVs
pvmove --abort

# check if proper pvmove was canceled
get lv_field $vg name -a | tee out
not grep "^\[pvmove" out
get lv_field $vg1 name -a | tee out
not grep "^\[pvmove" out

lvremove -ff $vg $vg1

wait
aux kill_tagged_processes
done
done

# Restore delayed device back
aux enable_dev "$dev3" "$dev6"

vgremove -ff $vg $vg1
