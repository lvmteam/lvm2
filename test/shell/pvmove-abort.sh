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

# Check pvmove --abort behaviour when specific device is requested

. lib/inittest

aux prepare_pvs 3 60

vgcreate -s 128k $vg "$dev1" "$dev2"
pvcreate --metadatacopies 0 "$dev3"
vgextend $vg "$dev3"

# Slowdown read/writes
aux delay_dev "$dev3" 0 800 $(get first_extent_sector "$dev3"):

for mode in "--atomic" "" ;
do
for backgroundarg in "-b" "" ;
do

# Create multisegment LV
lvcreate -an -Zn -l30 -n $lv1 $vg "$dev1"
lvcreate -an -Zn -l30 -n $lv2 $vg "$dev2"

cmd1=(pvmove -i1 $backgroundarg $mode "$dev1" "$dev3")
cmd2=(pvmove -i1 $backgroundarg $mode "$dev2" "$dev3")

if test -z "$backgroundarg" ; then
	"${cmd1[@]}" &
	aux wait_pvmove_lv_ready "$vg-pvmove0"
	"${cmd2[@]}" &
	aux wait_pvmove_lv_ready "$vg-pvmove1"
else
	LVM_TEST_TAG="kill_me_$PREFIX" "${cmd1[@]}"
	LVM_TEST_TAG="kill_me_$PREFIX" "${cmd2[@]}"
fi

# remove specific device
pvmove --abort "$dev1"

# check if proper pvmove was canceled
get lv_field $vg name -a | tee out
not grep "^\[pvmove0\]" out
grep "^\[pvmove1\]" out

# remove any remaining pvmoves in progress
pvmove --abort

lvremove -ff $vg

wait
aux kill_tagged_processes
done
done

# Restore delayed device back
aux enable_dev "$dev3"

vgremove -ff $vg
