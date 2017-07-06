#!/usr/bin/env bash

# Copyright (C) 2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Exercise fsadm operation on renamed device'
SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_vg 1 80

vg_lv=$vg/$lv1
vg_lv_ren=${vg_lv}_renamed

dev_vg_lv="$DM_DEV_DIR/$vg_lv"
dev_vg_lv_ren="$DM_DEV_DIR/$vg_lv_ren"

mount_dir="mnt"
mount_space_dir="mnt space dir"
mount_dolar_dir="mnt \$SPACE dir"
# for recursive call
LVM_BINARY=$(which lvm)
export LVM_BINARY

test ! -d "$mount_dir" && mkdir "$mount_dir"
test ! -d "$mount_space_dir" && mkdir "$mount_space_dir"
test ! -d "$mount_dolar_dir" && mkdir "$mount_dolar_dir"

cleanup_mounted_and_teardown()
{
	umount "$mount_dir" || true
	umount "$mount_space_dir" || true
	umount "$mount_dolar_dir" || true
	aux teardown
}

# Test for block sizes != 1024 (rhbz #480022)
trap 'cleanup_mounted_and_teardown' EXIT

# Iterate over supported filesystems
for i in mkfs.ext3 mkfs.xfs mkfs.reiserfs
do

if not which "$i" ; then
	echo "Skipping tests for missing $i"
	continue
fi

lvcreate -n $lv1 -L20M $vg

case "$i" in
*ext3)		MKFS_ARGS="-b1024 -j" ;;
*xfs)		MKFS_ARGS="-l internal,size=1000b -f" ;;
*reiserfs)	MKFS_ARGS="-s 513 -f" ;;
esac

echo "$i"
"$i" $MKFS_ARGS "$dev_vg_lv"

# Adding couple udev wait ops as some older systemd
# might get confused and was 'randomly/racy' umounting
# devices  just mounted.
# 
# See for explanation: 
#   https://github.com/systemd/systemd/commit/628c89cc68ab96fce2de7ebba5933725d147aecc
#   https://github.com/systemd/systemd/pull/2017
aux udev_wait

mount "$dev_vg_lv" "$mount_dir"

aux udev_wait

lvrename $vg_lv $vg_lv_ren

mount | tee out
grep $vg out

# fails on renamed LV
fail lvresize -L+10M -r $vg_lv_ren

# fails on unknown mountpoint  (FIXME: umount)
not umount "$dev_vg_lv"

lvcreate -L20 -n $lv1 $vg
"$i" $MKFS_ARGS "$dev_vg_lv"

aux udev_wait

mount "$dev_vg_lv" "$mount_dolar_dir"

mount | tee out
grep $vg out

not lvresize -L+10M -r $vg_lv_ren

umount "$mount_dir"

# FIXME:  lvresize  CANNOT handle/propagage '--yes' to fsadm
echo y | lvresize -L+10M -r $vg_lv

aux udev_wait

umount "$mount_dolar_dir"

lvremove -ff $vg

done

vgremove -ff $vg
