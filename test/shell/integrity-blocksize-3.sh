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

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_integrity 1 5 0 || skip

mnt="mnt"
mkdir -p $mnt

_sync_percent() {
        local checklv=$1
        get lv_field "$checklv" sync_percent | cut -d. -f1
}

_wait_recalc() {
        local checklv=$1

        for i in $(seq 1 10) ; do
                sync=$(_sync_percent "$checklv")
                echo "sync_percent is $sync"

                if test "$sync" = "100"; then
                        return
                fi

                sleep 1
        done

        # TODO: There is some strange bug, first leg of RAID with integrity
        # enabled never gets in sync. I saw this in BB, but not when executing
        # the commands manually
        if test -z "$sync"; then
                echo "TEST\ WARNING: Resync of dm-integrity device '$checklv' failed"
                dmsetup status "$DM_DEV_DIR/mapper/${checklv/\//-}"
                exit
        fi
        echo "timeout waiting for recalc"
        return 1
}

# scsi_debug devices with 512 LBS 512 PBS
aux prepare_scsi_debug_dev 256
check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "512"
check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "512"
aux prepare_devs 2 64

vgcreate $vg "$dev1" "$dev2"
blockdev --getss "$dev1"
blockdev --getpbsz "$dev1"
blockdev --getss "$dev2"
blockdev --getpbsz "$dev2"

# add integrity while LV is inactive
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvconvert --raidintegrity y $vg/$lv1
lvchange -ay $vg
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs unmounted
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvchange -ay $vg
lvconvert --raidintegrity y $vg/$lv1
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs mounted
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
lvconvert --raidintegrity y $vg/$lv1
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

vgremove -ff $vg
aux cleanup_scsi_debug_dev
sleep 1

# scsi_debug devices with 4K LBS and 4K PBS
aux prepare_scsi_debug_dev 256 sector_size=4096
check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "4096"
check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "4096"
aux prepare_devs 2 64

vgcreate $vg "$dev1" "$dev2"
blockdev --getss "$dev1"
blockdev --getpbsz "$dev1"
blockdev --getss "$dev2"
blockdev --getpbsz "$dev2"

# add integrity while LV is inactive
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvconvert --raidintegrity y $vg/$lv1
lvchange -ay $vg
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs unmounted
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvchange -ay $vg
lvconvert --raidintegrity y $vg/$lv1
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs mounted
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
lvconvert --raidintegrity y $vg/$lv1
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

vgremove -ff $vg
aux cleanup_scsi_debug_dev
sleep 1

# scsi_debug devices with 512 LBS and 4K PBS
aux prepare_scsi_debug_dev 256 sector_size=512 physblk_exp=3
check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "512"
check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "4096"
aux prepare_devs 2 64

vgcreate $vg "$dev1" "$dev2"
blockdev --getss "$dev1"
blockdev --getpbsz "$dev1"
blockdev --getss "$dev2"
blockdev --getpbsz "$dev2"

# add integrity while LV is inactive
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvconvert --raidintegrity y $vg/$lv1
lvchange -ay $vg
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs unmounted
# lvconvert will use ribs 512 to avoid increasing LBS from 512 to 4k on active LV
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
umount $mnt
lvchange -an $vg
lvchange -ay $vg
lvconvert --raidintegrity y $vg/$lv1
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# add integrity while LV is active, fs mounted
# lvconvert will use ribs 512 to avoid increasing LBS from 512 to 4k on active LV
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
echo "hello world" > $mnt/hello
lvconvert --raidintegrity y $vg/$lv1
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
cat $mnt/hello | grep "hello world"
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1

vgremove -ff $vg
aux cleanup_scsi_debug_dev
sleep 1

