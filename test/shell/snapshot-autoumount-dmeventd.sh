#!/bin/bash
# Copyright (C) 2010-2012 Red Hat, Inc. All rights reserved.
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

which mkfs.ext2 || skip

aux lvmconf "activation/snapshot_autoextend_percent = 0" \
            "activation/snapshot_autoextend_threshold = 100"

aux prepare_dmeventd
aux prepare_vg 2
mntdir="${PREFIX}mnt"

lvcreate -aey -L8 -n base $vg
mkfs.ext2 "$DM_DEV_DIR/$vg/base"

lvcreate -s -L4 -n snap $vg/base
lvchange --monitor y $vg/snap

mkdir "$mntdir"
mount "$DM_DEV_DIR/mapper/$vg-snap" "$mntdir"
mount
cat /proc/mounts | grep "$mntdir"
dd if=/dev/zero of="$mntdir/file$1" bs=1M count=5
sync
#dmeventd only checks every 10 seconds :(
for i in {1..10}; do
	cat /proc/mounts | grep "$mntdir" || break
	sleep 1
done

cat /proc/mounts | not grep "$mntdir"

vgremove -f $vg
