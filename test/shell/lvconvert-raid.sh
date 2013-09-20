#!/bin/sh
# Copyright (C) 2011-2012 Red Hat, Inc. All rights reserved.
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
if ! aux target_at_least dm-raid 1 2 0; then
	dmsetup targets | grep raid
	skip
fi

# 9 PVs needed for RAID10 testing (3-stripes/2-mirror - replacing 3 devs)
aux prepare_pvs 9 80
vgcreate -s 256k $vg $(cat DEVICES)

###########################################
# RAID1 convert tests
###########################################
for under_snap in false true; do
for i in 1 2 3; do
	for j in 1 2 3; do
		if [ $i -eq 1 ]; then
			from="linear"
		else
			from="$i-way"
		fi
		if [ $j -eq 1 ]; then
			to="linear"
		else
			to="$j-way"
		fi

		echo -n "Converting from $from to $to"
		if $under_snap; then
			echo -n " (while under a snapshot)"
		fi
		echo

		if [ $i -eq 1 ]; then
			# Shouldn't be able to create with just 1 image
			not lvcreate --type raid1 -m 0 -l 2 -n $lv1 $vg

			lvcreate -aey -l 2 -n $lv1 $vg
		else
			lvcreate --type raid1 -m $(($i - 1)) -l 2 -n $lv1 $vg
			aux wait_for_sync $vg $lv1
		fi

		if $under_snap; then
			lvcreate -aey -s $vg/$lv1 -n snap -l 2
		fi

		lvconvert -m $((j - 1)) $vg/$lv1

		# FIXME: ensure no residual devices

		if [ $j -eq 1 ]; then
			check linear $vg $lv1
		fi
		lvremove -ff $vg
	done
done
done

##############################################
# RAID1 - shouldn't be able to add image
#         if created '--nosync', but should
#         be able to after 'lvchange --resync'
##############################################
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg --nosync
not lvconvert -m +1 $vg/$lv1
lvchange --resync -y $vg/$lv1
aux wait_for_sync $vg $lv1
lvconvert -m +1 $vg/$lv1
lvremove -ff $vg

# 3-way to 2-way convert while specifying devices
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg $dev1 $dev2 $dev3
aux wait_for_sync $vg $lv1
lvconvert -m1 $vg/$lv1 $dev2
lvremove -ff $vg

#
# FIXME: Add tests that specify particular devices to be removed
#

###########################################
# RAID1 split tests
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check lv_exists $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

# 2-way to linear/linear
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check linear $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

###########################################
# RAID1 split + trackchanges / merge
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvconvert --splitmirrors 1 --trackchanges $vg/$lv1
check lv_exists $vg $lv1
check linear $vg ${lv1}_rimage_2
lvconvert --merge $vg/${lv1}_rimage_2
# FIXME: ensure no residual devices
lvremove -ff $vg

###########################################
# Linear to RAID1 conversion ("raid1" default segtype)
###########################################
lvcreate -aey -l 2 -n $lv1 $vg
lvconvert -m 1 $vg/$lv1 \
	--config 'global { mirror_segtype_default = "raid1" }'
lvs --noheadings -o attr $vg/$lv1 | grep '^r*'
lvremove -ff $vg

###########################################
# Linear to RAID1 conversion (override "mirror" default segtype)
###########################################
lvcreate -aey -l 2 -n $lv1 $vg
lvconvert --type raid1 -m 1 $vg/$lv1 \
	--config 'global { mirror_segtype_default = "mirror" }'
lvs --noheadings -o attr $vg/$lv1 | grep '^r*'
lvremove -ff $vg

###########################################
# Must not be able to convert non-EX LVs in a cluster
###########################################
if [ -e LOCAL_CLVMD ]; then
	lvcreate -l 2 -n $lv1 $vg
	not lvconvert --type raid1 -m 1 $vg/$lv1 \
		--config 'global { mirror_segtype_default = "mirror" }'
	lvremove -ff $vg
fi

###########################################
# Mirror to RAID1 conversion
###########################################
for i in 1 2 3 ; do
	lvcreate -aey --type mirror -m $i -l 2 -n $lv1 $vg
	aux wait_for_sync $vg $lv1
	lvconvert --type raid1 $vg/$lv1
	lvremove -ff $vg
done

###########################################
# Device Replacement Testing
###########################################
# RAID1: Replace up to n-1 devices - trying different combinations
# Test for 2-way to 4-way RAID1 LVs
for i in {1..3}; do
	lvcreate --type raid1 -m $i -l 2 -n $lv1 $vg

	for j in $(seq $(($i + 1))); do # The number of devs to replace at once
	for o in $(seq 0 $i); do        # The offset into the device list
		replace=""

		devices=( $(get_image_pvs $vg $lv1) )

		for k in $(seq $j); do
			index=$((($k + $o) % ($i + 1)))
			replace="$replace --replace ${devices[$index]}"
		done
		aux wait_for_sync $vg $lv1

		if [ $j -ge $((i + 1)) ]; then
			# Can't replace all at once.
			not lvconvert $replace $vg/$lv1
		else
			lvconvert $replace $vg/$lv1
		fi
	done
	done

	lvremove -ff $vg
done

aux skip_if_raid456_replace_broken

# RAID 4/5/6 (can replace up to 'parity' devices)
for i in 4 5 6; do
	lvcreate --type raid$i -i 3 -l 3 -n $lv1 $vg

	if [ $i -eq 6 ]; then
		dev_cnt=5
		limit=2
	else
		dev_cnt=4
		limit=1
	fi

	for j in {1..3}; do
	for o in $(seq 0 $i); do
		replace=""

		devices=( $(get_image_pvs $vg $lv1) )

		for k in $(seq $j); do
			index=$((($k + $o) % $dev_cnt))
			replace="$replace --replace ${devices[$index]}"
		done
		aux wait_for_sync $vg $lv1

		if [ $j -gt $limit ]; then
			not lvconvert $replace $vg/$lv1
		else
			lvconvert $replace $vg/$lv1
		fi
	done
	done

	lvremove -ff $vg
done
