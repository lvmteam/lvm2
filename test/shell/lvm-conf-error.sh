#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Check what happens when reading of lvm.conf fails

. lib/inittest

MKFS=mkfs.ext3
which $MKFS || skip
which filefrag || skip

aux prepare_vg 1

mkdir mnt

lvcreate -L5M -n $lv1 $vg

$MKFS "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" mnt
cp etc/lvm.conf mnt

# Figure where the file is placed in filesystem
filefrag -e mnt/lvm.conf | tee frags || rm -f frags
umount mnt

test -s frags || skip

# 1st. sector for filesystem
first_extent_sector=$(get first_extent_sector "$dev1")

# find 1st. 1k block of file and trim '..' from printed number
file_block=$(awk '/0:/ { gsub(/\.\.$/, "", $4); print $4}'  frags)

# figure sector position on DM device
file_sector=$(( file_block * 2 + first_extent_sector ))

aux error_dev "$dev1" $file_sector:2

mount "$DM_DEV_DIR/$vg/$lv1" mnt

# force lvm to read lvm.conf from mnt path
LVM_SYSTEM_DIR=mnt lvs 2>&1 | tee out || true

# shell give nice error message
grep "Failed to load config file mnt/lvm.conf" out

aux enable_dev "$dev1"

umount mnt

vgremove -ff $vg
