#!/usr/bin/env bash

# Copyright (C) 2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA2110-1301 USA

#######################################################################
# This series of tests is meant to validate the correctness of
# 'dmsetup status' for RAID LVs - especially during various sync action
# transitions, like: recover, resync, check, repair, idle, reshape, etc
#######################################################################
SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

export LVM_TEST_LVMETAD_DEBUG_OPTS=${LVM_TEST_LVMETAD_DEBUG_OPTS-}

. lib/inittest

# check for version 1.9.0
# - it is the point at which linear->raid1 uses "recover"
aux have_raid 1 9 0 || skip

aux prepare_pvs 9
get_devs

vgcreate -s 2m "$vg" "${DEVICES[@]}"

###########################################
# Upconverted RAID1 should never have all 'a's in status output
###########################################
aux delay_dev "$dev2" 0 50
lvcreate -aey -l 2 -n $lv1 $vg "$dev1"
lvconvert --type raid1 -y -m 1 $vg/$lv1 "$dev2"
while ! check in_sync $vg $lv1; do
        a=( $(dmsetup status $vg-$lv1) ) || die "Unable to get status of $vg/$lv1"
	[ "${a[5]}" != "aa" ]
        sleep .1
done
aux enable_dev "$dev2"
lvremove -ff $vg

###########################################
# Upconverted RAID1 should not be at 100% right after upconvert
###########################################
aux delay_dev "$dev2" 0 50
lvcreate -aey -l 2 -n $lv1 $vg "$dev1"
lvconvert --type raid1 -y -m 1 $vg/$lv1 "$dev2"
a=( $(dmsetup status $vg-$lv1) ) || die "Unable to get status of $vg/$lv1"
b=( $(echo "${a[6]}" | sed s:/:' ':) )
[ "${b[0]}" -ne "${b[1]}" ]
aux enable_dev "$dev2"
lvremove -ff $vg

###########################################
# Catch anything suspicious with linear -> RAID1 upconvert
###########################################
aux delay_dev "$dev2" 0 50
lvcreate -aey -l 2 -n $lv1 $vg "$dev1"
lvconvert --type raid1 -y -m 1 $vg/$lv1 "$dev2"
while true; do
        a=( $(dmsetup status $vg-$lv1) ) || die "Unable to get status of $vg/$lv1"
	b=( $(echo "${a[6]}" | sed s:/:' ':) )
	if [ "${b[0]}" -ne "${b[1]}" ]; then
		# If the sync operation ("recover" in this case) is not
		# finished, then it better be as follows:
		[ "${a[5]}" = "Aa" ]
		[ "${a[7]}" = "recover" ]
	else
		# Tough to tell the INVALID case,
		#   Before starting sync thread: "Aa X/X recover"
		# from the valid case,
		#   Just finished sync thread: "Aa X/X recover"
		[ "${a[5]}" = "AA" ]
		[ "${a[7]}" = "idle" ]
		break
	fi
        sleep .1
done
aux enable_dev "$dev2"
lvremove -ff $vg

###########################################
# Catch anything suspicious with RAID1 2-way -> 3-way upconvert
###########################################
aux delay_dev "$dev3" 0 50
lvcreate --type raid1 -m 1 -aey -l 2 -n $lv1 $vg "$dev1" "$dev2"
lvconvert -y -m +1 $vg/$lv1 "$dev3"
while true; do
        a=( $(dmsetup status $vg-$lv1) ) || die "Unable to get status of $vg/$lv1"
	b=( $(echo "${a[6]}" | sed s:/:' ':) )
	if [ "${b[0]}" -ne "${b[1]}" ]; then
		# If the sync operation ("recover" in this case) is not
		# finished, then it better be as follows:
		[ "${a[5]}" = "AAa" ]
		[ "${a[7]}" = "recover" ]
	else
		# Tough to tell the INVALID case,
		#   Before starting sync thread: "AAa X/X recover"
		# from the valid case,
		#   Just finished sync thread: "AAa X/X recover"
		[ "${a[5]}" = "AAA" ]
		[ "${a[7]}" = "idle" ]
		break
	fi
        sleep .1
done
aux enable_dev "$dev3"
lvremove -ff $vg

###########################################
# Catch anything suspicious with RAID1 initial resync
###########################################
aux delay_dev "$dev2" 0 50
lvcreate --type raid1 -m 1 -aey -l 2 -n $lv1 $vg "$dev1" "$dev2"
while true; do
        a=( $(dmsetup status $vg-$lv1) ) || die "Unable to get status of $vg/$lv1"
	b=( $(echo "${a[6]}" | sed s:/:' ':) )
	if [ "${b[0]}" -ne "${b[1]}" ]; then
		# If the sync operation ("resync" in this case) is not
		# finished, then it better be as follows:
		[ "${a[5]}" = "aa" ]
		[ "${a[7]}" = "resync" ]
	else
		[ "${a[5]}" = "AA" ]
		[ "${a[7]}" = "idle" ]
		break
	fi
        sleep .1
done
aux enable_dev "$dev2"
lvremove -ff $vg

vgremove -ff $vg
