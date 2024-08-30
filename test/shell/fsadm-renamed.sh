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

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_vg 1 700

vg_lv=$vg/$lv1
vg_lv_ren=${vg_lv}_renamed

dev_vg_lv="$DM_DEV_DIR/$vg_lv"
dev_vg_lv_ren="$DM_DEV_DIR/$vg_lv_ren"

mount_dir="mnt"
mount_space_dir="mnt space dir"
mount_dolar_dir="mnt \$SPACE dir"

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

check_mounted()
{
	mount | tee out
	grep $vg out || {
		# older versions of systemd sometimes umount volume by mistake
		# skip further test when this case happens
		systemctl --version | grep "systemd 222" && \
			skip "System is running old racy systemd version."
	}
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

lvcreate -n $lv1 -L300M $vg

case "$i" in
*ext3)		MKFS_ARGS="-b1024 -j" ;;
*xfs)		MKFS_ARGS="-l internal,size=64m -f" ;;
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

# mount /dev/test/lv1 on /mnt
mount "$dev_vg_lv" "$mount_dir"

aux udev_wait

# rename lv1 to lv1_renamed, now /dev/test/lv1_renamed is mounted on /mnt,
# but "df" and "mount" commands will still show /dev/test/lv1 mounted on /mnt.
lvrename $vg_lv $vg_lv_ren

check_mounted

# fails on renamed LV
# lvextend -r test/lv1_renamed succeeds in extending the LV (as lv1_renamed),
# but xfs_growfs /dev/test/lv1_renamed fails because it doesn't recognize
# that device is mounted, because the old lv name reported as being mounted.
fail lvresize -y -L+10M -r $vg_lv_ren

# fails on unknown mountpoint  (FIXME: umount)
not umount "$dev_vg_lv"

# create a new LV with the previous name of the renamed lv
lvcreate -L300 -n $lv1 $vg
"$i" $MKFS_ARGS "$dev_vg_lv"

aux udev_wait

# mount the new lv on a dir with a similar name as the other
# now df will show
# /dev/mapper/test-lv1 ... /mnt
# /dev/mapper/test-lv1 ... /mnt $SPACE dir
mount "$dev_vg_lv" "$mount_dolar_dir"

check_mounted

# try to resize the LV that was renamed:  lvextend -r test/lv1_renamed
# this succeeds in extending the LV (lv1_renamed), but xfs_growfs fails
# for the same reason as above, i.e. mount doesn't show the lv1_renamed
# device is mounted anywhere.
not lvresize -L+10M -r $vg_lv_ren

umount "$mount_dir"


USE_NOT=
# TODO: this is somewhat surprising for users
# Detect if the 'lvresize' was compiled with HAVE_BLKID_SUBLKS_FSINFO
# In such case --fs checksize is a supported parameter
# otherwise command automatically fallbacks to fsadm and resize reiserfs
not lvresize --fs checksize -L+1 $vg/XXX 2>err
if not grep "Unknown --fs value" err ; then
case "$i" in
# ATM  fsadm is required instead of '-r' option with reiserfs
*reiserfs) USE_NOT="not" ;;
esac
fi
$USE_NOT lvresize -y -L+10M -r $vg_lv

aux udev_wait

umount "$mount_dolar_dir"

lvremove -ff $vg

done

vgremove -ff $vg
