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

# Test single lv cache options

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 10 0 || skip
which mkfs.xfs || skip

mount_dir="mnt"
mkdir -p "$mount_dir"

# generate random data
dd if=/dev/urandom of=pattern1 bs=512K count=1

aux prepare_devs 5 64

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"

lvcreate -n $lv1 -l 8 -an $vg "$dev1"
lvcreate -n $lv2 -l 4 -an $vg "$dev2"
lvcreate -n $lv3 -l 4 -an $vg "$dev3"
lvcreate -n $lv4 -l 4 -an $vg "$dev4"
lvcreate -n $lv5 -l 8 -an $vg "$dev5"

mkfs_mount_umount()
{
	lvt=$1

	lvchange -ay $vg/$lvt

	mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lvt"
	mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
	cp pattern1 "$mount_dir/pattern1"
	dd if=/dev/zero of="$mount_dir/zeros2M" bs=1M count=2 conv=fdatasync
	umount "$mount_dir"

	lvchange -an $vg/$lvt
}

mount_umount()
{
	lvt=$1

	lvchange -ay $vg/$lvt

	mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
	diff pattern1 "$mount_dir/pattern1"
	dd if="$mount_dir/zeros2M" of=/dev/null bs=1M count=2
	umount "$mount_dir"

	lvchange -an $vg/$lvt
}

#
# Test --cachemetadataformat
#

# 1 shouldn't be used any longer
not lvconvert --cachemetadataformat 1 -y --type cache --cachevol $lv2 $vg/$lv1

# 3 doesn't exist
not lvconvert --cachemetadataformat 3 -y --type cache --cachevol $lv2 $vg/$lv1

# 2 is used by default
lvconvert -y --type cache --cachevol $lv2 $vg/$lv1

check lv_field $vg/$lv1 cachemetadataformat "2"

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear

# 2 can be set explicitly
lvconvert --cachemetadataformat 2 -y --type cache --cachevol $lv2 $vg/$lv1

check lv_field $vg/$lv1 cachemetadataformat "2"

lvconvert --splitcache $vg/$lv1

# "auto" means 2
lvconvert --cachemetadataformat auto -y --type cache --cachevol $lv2 $vg/$lv1

check lv_field $vg/$lv1 cachemetadataformat "2"

mkfs_mount_umount $lv1

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
mount_umount $lv1


#
# Test --poolmetadatasize
#

lvconvert -y --type cache --cachevol $lv2 --poolmetadatasize 4m $vg/$lv1

check lv_field $vg/$lv1 lv_metadata_size "4.00m"

mkfs_mount_umount $lv1

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
mount_umount $lv1


#
# Test --chunksize
#

lvconvert -y --type cache --cachevol $lv2 --chunksize 32k $vg/$lv1

check lv_field $vg/$lv1 chunksize "32.00k"

mkfs_mount_umount $lv1

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
mount_umount $lv1


#
# Test --cachemode
#

lvconvert -y --type cache --cachevol $lv2 --cachemode writethrough $vg/$lv1

check lv_field $vg/$lv1 cachemode "writethrough"

mkfs_mount_umount $lv1

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
mount_umount $lv1

# FIXME: kernel errors for other cache modes

#lvconvert -y --type cache --cachevol $lv2 --cachemode passthrough $vg/$lv1

#check lv_field $vg/$lv1 cachemode "passthrough"

#mkfs_mount_umount $lv1

#lvconvert --splitcache $vg/$lv1
#check lv_field $vg/$lv1 segtype linear
#check lv_field $vg/$lv2 segtype linear
#mount_umount $lv1


lvconvert -y --type cache --cachevol $lv2 --cachemode writeback $vg/$lv1

check lv_field $vg/$lv1 cachemode "writeback"

mkfs_mount_umount $lv1

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
mount_umount $lv1


#
# Test --cachepolicy
#

lvconvert -y --type cache --cachevol $lv2 --cachepolicy smq $vg/$lv1

check lv_field $vg/$lv1 cachepolicy "smq"

mkfs_mount_umount $lv1

lvchange --cachepolicy cleaner $vg/$lv1
lvchange -ay $vg/$lv1
check lv_field $vg/$lv1 cachepolicy "cleaner"
lvchange -an $vg/$lv1

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
mount_umount $lv1


#
# Test --cachesettings
# (only for mq policy, no settings for smq)
#

lvconvert -y --type cache --cachevol $lv2 --cachemode writethrough --cachepolicy mq --cachesettings 'migration_threshold = 233 sequential_threshold=13 random_threshold =1' $vg/$lv1

check lv_field $vg/$lv1 cachemode "writethrough"
check lv_field $vg/$lv1 cachepolicy "mq"

lvs -o cachesettings $vg/$lv1 > settings
grep "migration_threshold=233" settings
grep "sequential_threshold=13" settings
grep "random_threshold=1" settings

mkfs_mount_umount $lv1

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
mount_umount $lv1


#
# Test lvchange of --cachemode, --cachepolicy, --cachesettings
#

lvconvert -y --type cache --cachevol $lv2 $vg/$lv1

lvchange -ay $vg/$lv1

lvchange -y --cachemode writeback $vg/$lv1

check lv_field $vg/$lv1 cachemode "writeback"

lvchange --cachemode writethrough $vg/$lv1

check lv_field $vg/$lv1 cachemode "writethrough"

lvchange --cachemode passthrough $vg/$lv1

check lv_field $vg/$lv1 cachemode "passthrough"

lvchange -an $vg/$lv1

lvchange --cachepolicy mq --cachesettings 'migration_threshold=100' $vg/$lv1

check lv_field $vg/$lv1 cachepolicy "mq"
check lv_field $vg/$lv1 cachesettings "migration_threshold=100"

lvconvert --splitcache $vg/$lv1


#
# Test --poolmetadata
#

# causes a cache-pool type LV to be created
lvconvert -y --type cache --cachepool $lv3 --poolmetadata $lv4 $vg/$lv5

lvs -a -o+segtype $vg

check lv_field $vg/$lv5 segtype cache

# check lv_field doesn't work for hidden lvs
lvs -a -o segtype $vg/${lv3}_cpool > segtype
grep cache-pool segtype

lvconvert --splitcache $vg/$lv5
check lv_field $vg/$lv5 segtype linear
check lv_field $vg/$lv3 segtype cache-pool


vgremove -ff $vg
