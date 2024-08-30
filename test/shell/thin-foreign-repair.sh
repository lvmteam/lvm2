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

# test foreign user of thin-pool


SKIP_WITH_LVMPOLLD=1

. lib/inittest

clean_thin_()
{
	aux udev_wait
	dmsetup remove "$THIN" || { sleep .5 ; dmsetup remove "$THIN" ; }
}

cleanup_mounted_and_teardown()
{
	clean_thin_ || true
	vgremove -ff $vg
	aux teardown
}

#
# Main
#
aux have_thin 1 0 0 || skip
which mkfs.ext4 || skip

# Use our mkfs config file to get approximately same results
# TODO: maybe use it for all test via some 'prepare' function
export MKE2FS_CONFIG="$TESTOLDPWD/lib/mke2fs.conf"

aux prepare_vg 2 64

# Create named pool only
lvcreate -L2 -T $vg/pool

POOL="$vg-pool"
THIN="${PREFIX}_thin"

# Foreign user is using own ioctl command to create thin devices
dmsetup message $POOL 0 "create_thin 0"
dmsetup message $POOL 0 "set_transaction_id 0 2"

# Once the transaction id has changed, lvm2 shall not be able to create thinLV
fail lvcreate -V10 $vg/pool

trap 'cleanup_mounted_and_teardown' EXIT

# 20M thin device
dmsetup create "$THIN" --table "0 40960 thin $DM_DEV_DIR/mapper/$POOL 0"

mkfs.ext4 "$DM_DEV_DIR/mapper/$THIN"

clean_thin_

lvchange -an $vg/pool

# Repair thin-pool used by 'foreing' apps (setting their own tid)
lvconvert --repair $vg/pool 2>&1 | tee out

not grep "Transaction id" out

lvchange -ay $vg/pool

dmsetup create "$THIN" --table "0 40960 thin $DM_DEV_DIR/mapper/$POOL 0"

fsck -n "$DM_DEV_DIR/mapper/$THIN"

# exit calls cleanup_mounted_and_teardown
