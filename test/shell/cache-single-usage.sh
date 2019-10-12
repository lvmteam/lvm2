#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test single lv cache

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 10 0 || skip
which mkfs.xfs || skip

mount_dir="mnt"
mkdir -p "$mount_dir"

# generate random data
dd if=/dev/urandom of=pattern1 bs=512K count=1

aux prepare_devs 2 64

vgcreate $SHARED $vg "$dev1"

vgextend $vg "$dev2"

lvcreate -n $lv1 -l 8 -an $vg "$dev1"

lvcreate -n $lv2 -l 4 -an $vg "$dev2"

# test1: create fs on LV before cache is attached

lvchange -ay $vg/$lv1

mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv1"

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

cp pattern1 "$mount_dir/pattern1"

umount "$mount_dir"
lvchange -an $vg/$lv1

lvconvert -y --type cache --cachevol $lv2 $vg/$lv1

check lv_field $vg/$lv1 segtype cache

lvs -a $vg/${lv2}_cvol --noheadings -o segtype >out
grep linear out

lvchange -ay $vg/$lv1

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

diff pattern1 "$mount_dir/pattern1"

cp pattern1 "$mount_dir/pattern1b"

ls -l "$mount_dir"

umount "$mount_dir"

lvchange -an $vg/$lv1

lvconvert --splitcache $vg/$lv1

check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear

lvchange -ay $vg/$lv1
lvchange -ay $vg/$lv2

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

ls -l "$mount_dir"

diff pattern1 "$mount_dir/pattern1"
diff pattern1 "$mount_dir/pattern1b"

umount "$mount_dir"
lvchange -an $vg/$lv1
lvchange -an $vg/$lv2

# test2: create fs on LV after cache is attached

lvconvert -y --type cache --cachevol $lv2 $vg/$lv1

check lv_field $vg/$lv1 segtype cache

lvs -a $vg/${lv2}_cvol --noheadings -o segtype >out
grep linear out

lvchange -ay $vg/$lv1

mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv1"

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

cp pattern1 "$mount_dir/pattern1"
ls -l "$mount_dir"

umount "$mount_dir"
lvchange -an $vg/$lv1

lvconvert --splitcache $vg/$lv1

check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear

lvchange -ay $vg/$lv1
lvchange -ay $vg/$lv2

mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

ls -l "$mount_dir"

diff pattern1 "$mount_dir/pattern1"

umount "$mount_dir"
lvchange -an $vg/$lv1
lvchange -an $vg/$lv2

vgremove -ff $vg
