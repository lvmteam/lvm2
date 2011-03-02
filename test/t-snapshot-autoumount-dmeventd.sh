#!/bin/bash
# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# no automatic extensions please

. lib/test

which mkfs.ext2 || exit 200

aux lvmconf "activation/snapshot_autoextend_percent = 0" \
            "activation/snapshot_autoextend_threshold = 100"

aux prepare_vg 2
aux prepare_dmeventd

lvcreate -l 8 -n base $vg
mkfs.ext2 $DM_DEV_DIR/$vg/base

lvcreate -s -l 4 -n snap $vg/base
lvchange --monitor y $vg/snap

mkdir mnt
mount $DM_DEV_DIR/mapper/$vg-snap mnt
mount
cat /proc/mounts | grep $vg-snap

dd if=/dev/zero of=mnt/file$1 bs=1M count=17
sync
sleep 10 # dmeventd only checks every 10 seconds :(

cat /proc/mounts | not grep $vg-snap
