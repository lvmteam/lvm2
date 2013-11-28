#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# 'Exercise signature wiping during lvcreate'

. lib/test

aux prepare_devs 1
aux pvcreate -f $dev1
aux vgcreate $vg $dev1

# lvcreate wipes signatures when found on newly created LV - test this on "swap".
# Test all combinatios with -Z{y|n} and -W{y|n} and related lvm.conf settings.

lvcreate -l1 -n $lv1 $vg
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

wiping_msg="Wiping swap signature"

aux lvmconf "allocation/wipe_signatures_on_new_logical_volumes_when_zeroing = 0"

lvcreate -y -Zn -l1 -n $lv1 $vg | not grep "$wiping_msg"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wn -l1 -n $lv1 $vg | not grep "$wiping_msg"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wy -l1 -n $lv1 $vg | grep "$wiping_msg"
(blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" || true) | not grep "swap"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zy -l1 -n $lv1 $vg | not grep "$wiping_msg"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wn -l1 -n $lv1 $vg | not grep "$wiping_msg"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wy -l1 -n $lv1 $vg | grep "$wiping_msg"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1


aux lvmconf "allocation/wipe_signatures_on_new_logical_volumes_when_zeroing = 1"

lvcreate -y -Zn -l1 -n $lv1 $vg | not grep "$wiping_msg"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wn -l1 -n $lv1 $vg | not grep "$wiping_msg"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zn -Wy -l1 -n $lv1 $vg | grep "$wiping_msg"
(blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" || true) | not grep "swap"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zy -l1 -n $lv1 $vg | grep "$wiping_msg"
(blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" || true) | not grep "swap"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wn -l1 -n $lv1 $vg | not grep "$wiping_msg"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1

lvcreate -y -Zy -Wy -l1 -n $lv1 $vg | grep "$wiping_msg"
(blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" || true) | not grep "swap"
mkswap "$DM_DEV_DIR/$vg/$lv1"
blkid -c /dev/null "$DM_DEV_DIR/$vg/$lv1" | grep "swap"
lvremove -f $vg/$lv1


vgremove $vg
pvremove $dev1
