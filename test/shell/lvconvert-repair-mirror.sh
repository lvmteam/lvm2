#!/usr/bin/env bash

# Copyright (C) 2016-2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

MOUNT_DIR=mnt
MKFS=$(which mkfs.ext3) || skip

cleanup_mounted_and_teardown()
{
	umount "$MOUNT_DIR" || true
	aux teardown
}

aux lvmconf 'allocation/mirror_logs_require_separate_pvs = 1'

aux prepare_vg 5

mkdir -p "$MOUNT_DIR"

trap 'cleanup_mounted_and_teardown' EXIT

################### Check repair for lost mirror leg and log #################
#
for i in "$dev2" "$dev3"
do

lvcreate -aey --type mirror -L10 --regionsize 64k -m1 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
"$MKFS" "$DM_DEV_DIR/$vg/$lv1"

aux delay_dev "$i" 0 10 "$(get first_extent_sector "$i"):"
#
# Enforce synchronization
# ATM requires unmounted/unused LV??
#
lvchange --yes --resync $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" "$MOUNT_DIR"

# run 'dd' operation during failure of 'mlog/mimage' device

dd if=/dev/zero of=mnt/zero bs=4K count=100 conv=fdatasync 2>err &
DD_PID=$!

PERCENT=$(get lv_field $vg/$lv1 copy_percent)
PERCENT=${PERCENT%%\.*}  # cut decimal
# and check less than 50% mirror is in sync (could be unusable delay_dev ?)
test "$PERCENT" -lt 50 || skip
#lvs -a -o+devices $vg

aux disable_dev "$i"

lvconvert --yes --repair $vg/$lv1

aux enable_dev "$i"
vgck --updatemetadata $vg

wait "$DD_PID" || true
# dd MAY NOT HAVE produced any error message
not grep error err

lvs -a -o+devices $vg
umount "$MOUNT_DIR"
fsck -n "$DM_DEV_DIR/$vg/$lv1"

lvremove -ff $vg

done
