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

# 'Exercise signature wiping during lvcreate'



. lib/inittest --skip-with-lvmpolld

_init_lv() {
	mkswap "$DM_DEV_DIR/$vg/$lv1"
}

_is_swap() {
	local type
	# for empty devices without any types blkid exits with return code 2
	type=$(blkid -s TYPE -o value -c /dev/null "$DM_DEV_DIR/$vg/$lv1") || true
	test "$type" = "swap"
}

_is_not_swap() {
	local type
	# for empty devices without any types blkid exits with return code 2
	type=$(blkid -s TYPE -o value -c /dev/null "$DM_DEV_DIR/$vg/$lv1") || true
	[[ "$type" != "swap" ]]
}

_was_wiping() {
	grep "Wiping swap signature" out
}

_was_not_wiping() {
	not grep "Wiping swap signature" out
}

aux prepare_vg

# lvcreate wipes signatures when found on newly created LV - test this on "swap".
# Test all combinations with -Z{y|n} and -W{y|n} and related lvm.conf settings.

lvcreate -l1 -n $lv1 $vg
_init_lv
# This system has unusable blkid (does not recognize small swap, needs fix...)
_is_swap || skip
lvremove -f $vg/$lv1

# Zeroing stops the command when there is a failure (write error in this case)
aux error_dev "$dev1" "$(get first_extent_sector "$dev1"):8"
not lvcreate -l1 -n $lv1 $vg 2>&1 | tee out
grep "Failed to initialize" out
aux enable_dev "$dev1"


aux lvmconf "allocation/wipe_signatures_when_zeroing_new_lvs = 0"

lvcreate -y -Zn -l1 -n $lv1 $vg 2>&1 | tee out
_was_not_wiping
_is_swap
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wn -l1 -n $lv1 $vg 2>&1 | tee out
_was_not_wiping
_is_swap
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wy -l1 -n $lv1 $vg 2>&1 | tee out
_was_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

lvcreate -y -Zy -l1 -n $lv1 $vg 2>&1 | tee out
_was_not_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wn -l1 -n $lv1 $vg 2>&1 | tee out
_was_not_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wy -l1 -n $lv1 $vg 2>&1 | tee out
_was_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

aux lvmconf "allocation/wipe_signatures_when_zeroing_new_lvs = 1"

lvcreate -y -Zn -l1 -n $lv1 $vg 2>&1 | tee out
_was_not_wiping
_is_swap
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wn -l1 -n $lv1 $vg 2>&1 | tee out
_was_not_wiping
_is_swap
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wy -l1 -n $lv1 $vg 2>&1 | tee out
_was_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

lvcreate -y -Zy -l1 -n $lv1 $vg 2>&1 | tee out
_was_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wn -l1 -n $lv1 $vg 2>&1 | tee out
_was_not_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wy -l1 -n $lv1 $vg 2>&1 | tee out
_was_wiping
_is_not_swap
_init_lv
lvremove -f $vg/$lv1

vgremove -f $vg

aux udev_wait

aux udev_wait

aux udev_wait
