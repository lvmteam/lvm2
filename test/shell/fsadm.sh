#!/bin/bash
# Copyright (C) 2008-2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Exercise fsadm filesystem resize'

. lib/test

aux prepare_vg 1 100

# set to "skip" to avoid testing given fs and test warning result
# i.e. check_reiserfs=skip
check_ext3=
check_xfs=
check_reiserfs=

which mkfs.ext3 || check_ext3=${check_ext3:=mkfs.ext3}
which fsck.ext3 || check_ext3=${check_ext3:=fsck.ext3}
which mkfs.xfs || check_xfs=${check_xfs:=mkfs.xfs}
which xfs_check || check_xfs=${check_xfs:=xfs_check}
which mkfs.reiserfs || check_reiserfs=${check_reiserfs:=mkfs.reiserfs}
which reiserfsck || check_reiserfs=${check_reiserfs:=reiserfsck}

vg_lv="$vg/$lv1"
dev_vg_lv="$DM_DEV_DIR/$vg_lv"
mount_dir="$TESTDIR/mnt"
# for recursive call
export LVM_BINARY=$(which lvm)

test ! -d $mount_dir && mkdir $mount_dir

cleanup_mounted_and_teardown()
{
	umount $mount_dir || true
	aux teardown
}

fscheck_ext3()
{
	fsck.ext3 -p -F -f $dev_vg_lv
}

fscheck_xfs()
{
	xfs_check $dev_vg_lv
}

fscheck_reiserfs()
{
	reiserfsck --check -p -f $dev_vg_lv </dev/null
}

check_missing()
{
	eval local t=$\check_$1
	test -z "$t" && return 0
	test "$t" = skip && return 1
	# trick for warning test
	echo "TEST ""WARNING: fsadm skips $1 tests, $t tool is missing"
	return 1
}

# Test for block sizes != 1024 (rhbz #480022)
lvcreate -n $lv1 -L20M $vg
trap 'cleanup_mounted_and_teardown' EXIT

if check_missing ext3; then
	mkfs.ext3 -b4096 -j $dev_vg_lv

	fsadm --lvresize resize $vg_lv 30M
	# Fails - not enough space for 4M fs
	not fsadm -y --lvresize resize $dev_vg_lv 4M
	lvresize -L+10M -r $vg_lv
	lvreduce -L10M -r $vg_lv

	fscheck_ext3
	mount $dev_vg_lv $mount_dir
	not fsadm -y --lvresize resize $vg_lv 4M
	echo n | not lvresize -L4M -r -n $vg_lv
	lvresize -L+20M -r -n $vg_lv
	umount $mount_dir
	fscheck_ext3

	lvresize -f -L20M $vg_lv
fi

if check_missing xfs; then
	mkfs.xfs -l internal,size=1000b -f $dev_vg_lv

	fsadm --lvresize resize $vg_lv 30M
	# Fails - not enough space for 4M fs
	lvresize -L+10M -r $vg_lv
	not lvreduce -L10M -r $vg_lv

	fscheck_xfs
	mount $dev_vg_lv $mount_dir
	lvresize -L+10M -r -n $vg_lv
	umount $mount_dir
	fscheck_xfs

	lvresize -f -L20M $vg_lv
fi

if check_missing reiserfs; then
	mkfs.reiserfs -s 513 -f $dev_vg_lv

	fsadm --lvresize resize $vg_lv 30M
	lvresize -L+10M -r $vg_lv
	fsadm --lvresize -y resize $vg_lv 10M

	fscheck_reiserfs
	mount $dev_vg_lv $mount_dir

	fsadm -y --lvresize resize $vg_lv 30M
	umount $mount_dir
	fscheck_reiserfs

	lvresize -f -L20M $vg_lv
fi

vgremove -ff $vg
