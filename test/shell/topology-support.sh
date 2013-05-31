#!/bin/sh
# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

which mkfs.ext3 || skip

check_logical_block_size() {
    local DEV_=$(cat SCSI_DEBUG_DEV)
    # Verify logical_block_size - requires Linux >= 2.6.31
    SYSFS_LOGICAL_BLOCK_SIZE=$(echo /sys/block/$(basename $DEV_)/queue/logical_block_size)
    if [ -f "$SYSFS_LOGICAL_BLOCK_SIZE" ] ; then
	ACTUAL_LOGICAL_BLOCK_SIZE=$(cat $SYSFS_LOGICAL_BLOCK_SIZE)
	test $ACTUAL_LOGICAL_BLOCK_SIZE = $1
    fi
}

check_optimal_io_size() {
    local DEV_=$(cat SCSI_DEBUG_DEV)
    # Verify optimal_io_size
    SYSFS_OPTIMAL_IO_SIZE=$(echo /sys/block/$(basename $DEV_)/queue/optimal_io_size)
    if [ -f "$SYSFS_OPTIMAL_IO_SIZE" ] ; then
	ACTUAL_OPTIMAL_IO_SIZE=$(cat $SYSFS_OPTIMAL_IO_SIZE)
	test $ACTUAL_OPTIMAL_IO_SIZE = $1
    fi
}

lvdev_() {
    echo "$DM_DEV_DIR/$1/$2"
}

test_snapshot_mount() {
    lvcreate -aey -L4M -n $lv1 $vg "$dev1"
    mkfs.ext3 $(lvdev_ $vg $lv1)
    mkdir test_mnt
    mount "$(lvdev_ $vg $lv1)" test_mnt
    lvcreate -L4M -n $lv2 -s $vg/$lv1
    umount test_mnt
    # mount the origin
    mount "$(lvdev_ $vg $lv1)" test_mnt
    umount test_mnt
    # mount the snapshot
    mount "$(lvdev_ $vg $lv2)" test_mnt
    umount test_mnt
    rm -r test_mnt
    vgchange -an $vg
    lvremove -f $vg/$lv1
}

# FIXME add more topology-specific tests and validation (striped LVs, etc)

NUM_DEVS=1
PER_DEV_SIZE=34
DEV_SIZE=$(($NUM_DEVS*$PER_DEV_SIZE))

# Test that kernel supports topology
aux prepare_scsi_debug_dev $DEV_SIZE || skip

if [ ! -e /sys/block/$(basename $(cat SCSI_DEBUG_DEV))/alignment_offset ] ; then
	aux cleanup_scsi_debug_dev
	skip
fi
aux cleanup_scsi_debug_dev

# ---------------------------------------------
# Create "desktop-class" 4K drive
# (logical_block_size=512, physical_block_size=4096, alignment_offset=0):
LOGICAL_BLOCK_SIZE=512
aux prepare_scsi_debug_dev $DEV_SIZE \
    sector_size=$LOGICAL_BLOCK_SIZE physblk_exp=3
check_logical_block_size $LOGICAL_BLOCK_SIZE

aux prepare_pvs $NUM_DEVS $PER_DEV_SIZE
vgcreate $vg $(cat DEVICES)
test_snapshot_mount
vgremove $vg

aux cleanup_scsi_debug_dev

# ---------------------------------------------
# Create "desktop-class" 4K drive w/ 63-sector DOS partition compensation
# (logical_block_size=512, physical_block_size=4096, alignment_offset=3584):
LOGICAL_BLOCK_SIZE=512
aux prepare_scsi_debug_dev $DEV_SIZE \
    sector_size=$LOGICAL_BLOCK_SIZE physblk_exp=3 lowest_aligned=7
check_logical_block_size $LOGICAL_BLOCK_SIZE

aux prepare_pvs $NUM_DEVS $PER_DEV_SIZE
vgcreate $vg $(cat DEVICES)
test_snapshot_mount
vgremove $vg

aux cleanup_scsi_debug_dev

# ---------------------------------------------
# Create "enterprise-class" 4K drive
# (logical_block_size=4096, physical_block_size=4096, alignment_offset=0):
LOGICAL_BLOCK_SIZE=4096
aux prepare_scsi_debug_dev $DEV_SIZE \
    sector_size=$LOGICAL_BLOCK_SIZE
check_logical_block_size $LOGICAL_BLOCK_SIZE

aux prepare_pvs $NUM_DEVS $PER_DEV_SIZE
vgcreate $vg $(cat DEVICES)
test_snapshot_mount
vgremove $vg

aux cleanup_scsi_debug_dev

# ---------------------------------------------
# Create "enterprise-class" 512 drive w/ HW raid stripe_size = 768K
# (logical_block_size=512, physical_block_size=512, alignment_offset=0):
# - tests case where optimal_io_size=768k < default PE alignment=1MB
LOGICAL_BLOCK_SIZE=512
aux prepare_scsi_debug_dev $DEV_SIZE \
    sector_size=$LOGICAL_BLOCK_SIZE opt_blks=1536
check_logical_block_size $LOGICAL_BLOCK_SIZE
check_optimal_io_size 786432

aux prepare_pvs 1 $PER_DEV_SIZE
check pv_field $(cat DEVICES) pe_start 768.00k

aux cleanup_scsi_debug_dev
