#!/bin/bash
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# no automatic extensions, just umount

is_dir_mounted_()
{
	cat /proc/mounts | sed 's:\\040: :g' | grep "$1"
}

. lib/test

#
# Main
#
which mkfs.ext2 || skip

aux have_thin 1 0 0 || skip

aux prepare_dmeventd

aux lvmconf "activation/thin_pool_autoextend_percent = 0" \
            "activation/thin_pool_autoextend_threshold = 70"

aux prepare_vg 2

mntdir="${PREFIX}mnt with space"
mntusedir="${PREFIX}mntuse"

lvcreate -L8M -V8M -n $lv1 -T $vg/pool
lvcreate -V8M -n $lv2 -T $vg/pool

mkfs.ext2 "$DM_DEV_DIR/$vg/$lv1"
mkfs.ext2 "$DM_DEV_DIR/$vg/$lv2"

lvchange --monitor y $vg/pool

mkdir "$mntdir" "$mntusedir"
mount "$DM_DEV_DIR/mapper/$vg-$lv1" "$mntdir"
mount "$DM_DEV_DIR/mapper/$vg-$lv2" "$mntusedir"

is_dir_mounted_ "$mntdir"

# fill above 70%
dd if=/dev/zero of="$mntdir/file$$" bs=1M count=6
touch "$mntusedir/file$$"
tail -f "$mntusedir/file$$" &
PID_TAIL=$!
sync
lvs -a $vg
sleep 12 # dmeventd only checks every 10 seconds :(

lvs -a $vg
# both dirs should be unmounted
not is_dir_mounted "$mntdir"
not is_dir_mounted "$mntusedir"

# running tail keeps the block device still in use
kill $PID_TAIL
lvs -a $vg

vgremove -f $vg
