#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

get_image_pvs() {
	local d
	local images

	images=`dmsetup ls | grep ${1}-${2}_.image_.* | cut -f1 | sed -e s:-:/:`
	lvs --noheadings -a -o devices $images | sed s/\(.\)//
}

########################################################
# MAIN
########################################################
# RAID10: Can replace 'copies - 1' devices from each stripe
# Tests are run on 2-way mirror, 3-way stripe RAID10
aux target_at_least dm-raid 1 3 1 || skip

# 9 PVs needed for RAID10 testing (3-stripes/2-mirror - replacing 3 devs)
aux prepare_pvs 9 80
vgcreate -s 256k $vg $(cat DEVICES)

lvcreate --type raid10 -m 1 -i 3 -l 3 -n $lv1 $vg
aux wait_for_sync $vg $lv1

# Can replace any single device
for i in $(get_image_pvs $vg $lv1); do
	lvconvert --replace $i $vg/$lv1
	aux wait_for_sync $vg $lv1
done

# Can't replace adjacent devices
devices=( $(get_image_pvs $vg $lv1) )
not lvconvert --replace ${devices[0]} --replace ${devices[1]} $vg/$lv1
not lvconvert --replace ${devices[2]} --replace ${devices[3]} $vg/$lv1
not lvconvert --replace ${devices[4]} --replace ${devices[5]} $vg/$lv1

# Can replace non-adjacent devices
for i in 0 1; do
	lvconvert \
		--replace ${devices[$i]} \
		--replace ${devices[$(($i + 2))]} \
		--replace ${devices[$(($i + 4))]} \
		 $vg/$lv1
	aux wait_for_sync $vg $lv1
done

lvremove -ff $vg
