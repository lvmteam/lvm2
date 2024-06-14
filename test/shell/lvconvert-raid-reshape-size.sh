#!/usr/bin/env bash

# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA2110-1301 USA

LVM_SKIP_LARGE_TESTS=1
SKIP_RESIZE=0

. lib/inittest

which mkfs.ext4 || skip
[ $SKIP_RESIZE -eq 0 ] && ( which resize2fs || skip )

aux kernel_at_least 6 9 0 || skip

test "$(aux total_mem)" -gt 1048576 || skip "Not enough RAM for this test"

# List of stripes to add or remove
tst_stripes=""

# Delay on first 'PV' to be able to retrieve status during reshape.
# Needs to be varied depending on layout as they come with different reshape overhead.
ms=0

# PV numbers and size
npvs=0
pvsz=0

function _get_pvs_stripes_and_delay
{
	local raid_type=$1

	case $LVM_SKIP_LARGE_TESTS in
	0)
		npvs=64
		pvsz=32

		case $raid_type in
		raid[45]*)
			ms=30
			tst_stripes="5 4 11 9 15 13 19 18 22 21 25 23 28 30 31 29 33 31 34 33 37 45 41 50 55 60 63 30 15 10 8 3"
			;;
		raid6*)
			ms=20
			tst_stripes="5 4 11 9 15 13 19 18 22 21 25 23 28 30 31 29 33 31 34 33 37 45 41 50 55 60 62 30 15 10 8 3"
			;;
		raid10*)
			ms=50
			tst_stripes="5 8 9 11 13 14 15 19 20 22 23 25 27 28 29 32"
			;;
		esac
		;;
	*)
		npvs=20
		pvsz=16

		case $raid_type in
		raid[45]*)
			ms=40
			tst_stripes="5 4 8 7 11 9 15 19 18 10"
			;;
		raid6*)
			ms=25
			tst_stripes="5 4 8 7 11 9 15 18 10 5"
			;;
		raid10*)
			ms=40
			tst_stripes="5 6 7 8 9 10"
			;;
		esac
	esac
}

function _get_size
{
	local vg=$1
	local lv=$2
	local data_stripes=$3
	local reshape_len rimagesz

        # Get any reshape size in sectors
	reshape_len=$(lvs --noheadings -aoname,reshapelen --unit s $vg/${lv}_rimage_0|head -1|cut -d ']' -f2)
	reshape_len=$(echo ${reshape_len/S}|xargs)

	# Get rimage size - reshape length
	rimagesz=$(($(blockdev --getsz /dev/mapper/${vg}-${lv}_rimage_0) - $reshape_len))

	# Calculate size of LV based on above sizes
	echo $(($rimagesz * $data_stripes))
}

function _check_size
{
	local vg=$1
	local lv=$2
	local data_stripes=$3

	# Compare size of LV with calculated one
	[ $(blockdev --getsz /dev/$vg/$lv) -eq $(_get_size $vg $lv $data_stripes) ] && echo 0 || echo 1
}

function _total_stripes
{
	local raid_type=$1
	local data_stripes=$2

	case $raid_type in
	raid[45]*) echo $(($data_stripes + 1)) ;;
	raid6*)    echo $(($data_stripes + 2)) ;;
	raid10*)   echo $(($data_stripes * 2)) ;;
	esac
}

function _lvcreate
{
	local raid_type=$1
	local data_stripes=$2
	local size=$3
	local vg=$4
	local lv=$5
	shift 5
	local opts="$*"
	local stripes=$(_total_stripes $raid_type $data_stripes)

	lvcreate -y -aey --type $raid_type -i $data_stripes -L $size -n $lv $vg $opts

	check lv_first_seg_field $vg/$lv segtype "$raid_type"
	check lv_first_seg_field $vg/$lv datastripes $data_stripes
	check lv_first_seg_field $vg/$lv stripes $stripes

	mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
	fsck -fy "$DM_DEV_DIR/$vg/$lv"
}

function _reshape_layout
{
	local raid_type=$1
	local data_stripes=$2
	local vg=$3
	local lv=$4
	local wait_for_reshape=$5
	local ignore_a_chars=$6
	shift 6
	local opts="$*"
	local stripes=$(_total_stripes $raid_type $data_stripes)

	# Avoid random udev sync delays causing _check_size to be unreliable
	lvconvert -y --noudevsync --ty $raid_type --stripes $data_stripes $opts $vg/$lv
	check lv_first_seg_field $vg/$lv1 segtype "$raid_type"

	if [ $wait_for_reshape -eq 1 ]
	then
		aux wait_for_sync $vg $lv $ignore_a_chars
		fsck -fn "$DM_DEV_DIR/$vg/$lv"
	fi
}

function _add_stripes
{
	local raid_type=$1
	local vg=$2
	local lv=$3
	local data_stripes=$4
	local stripes=$(_total_stripes $raid_type $data_stripes)
	local stripesize="$((16 << ($data_stripes % 5))).00k" # Stripe size variation

	aux delay_dev "$dev1" $ms 0
	_reshape_layout $raid_type $data_stripes $vg $lv 0 1 --stripesize $stripesize

	# Size has to be inconsistent until reshape finishes
	[ $(_check_size $vg $lv $data_stripes) -ne 0 ] || die "LV size should be small"

	check lv_first_seg_field $vg/$lv stripesize "$stripesize"
	check lv_first_seg_field $vg/$lv datastripes $data_stripes
	check lv_first_seg_field $vg/$lv stripes $stripes

	fsck -fy "$DM_DEV_DIR/$vg/$lv"
	aux delay_dev "$dev1" 0 0
	aux wait_for_sync $vg $lv 0
	sleep 1

	# Now size consistency has to be fine
	[ $(_check_size $vg $lv $data_stripes) -eq 0 ] || die "LV size should be grown"

	# Check, use grown capacity for the filesystem and check again
	if [ $SKIP_RESIZE -eq 0 ]
	then
		fsck -fy "$DM_DEV_DIR/$vg/$lv"
		resize2fs "$DM_DEV_DIR/$vg/$lv"
	fi

	fsck -fy "$DM_DEV_DIR/$vg/$lv"

	udevadm settle
}

function _remove_stripes
{
	local raid_type=$1
	local vg=$2
	local lv=$3
	local data_stripes=$4
	local cur_data_stripes=$(get lv_field "$vg/$lv" datastripes -a)
	local stripes=$(get lv_field "$vg/$lv" stripes -a)
	local stripesize="$((16 << ($data_stripes % 5))).00k" # Stripe size variation

	# Check, shrink hilesystem to the resulting smaller size and check again
	if [ $SKIP_RESIZE -eq 0 ]
	then
		fsck -fy "$DM_DEV_DIR/$vg/$lv"
		resize2fs "$DM_DEV_DIR/$vg/$lv" $(_get_size $vg $lv $data_stripes)s
		fsck -fy "$DM_DEV_DIR/$vg/$lv"
	fi

	aux delay_dev "$dev1" $ms 0
	_reshape_layout $raid_type $data_stripes $vg $lv 0 1 --force --stripesize $stripesize

	# Size has to be inconsistent, as to be removed legs still exist
	[ $(_check_size $vg $lv $cur_data_stripes) -ne 0 ] || die "LV size should be reduced but not rimage count"

	check lv_first_seg_field $vg/$lv stripesize "$stripesize"
	check lv_first_seg_field $vg/$lv datastripes $data_stripes
	check lv_first_seg_field $vg/$lv stripes $stripes

	fsck -fy "$DM_DEV_DIR/$vg/$lv"

	# Have to remove freed legs before another restriping conversion. Will fail while reshaping is ongoing as stripes are still in use
	not _reshape_layout $raid_type $(($data_stripes + 1)) 0 1 $vg $lv --force
	aux delay_dev "$dev1" 0 0
	aux wait_for_sync $vg $lv 1

	# Remove freed legs as they are now idle has to succeed without --force
	_reshape_layout $raid_type $data_stripes $vg $lv 1 1
	check lv_first_seg_field $vg/$lv datastripes $data_stripes
	check lv_first_seg_field $vg/$lv stripes $(_total_stripes $raid_type $data_stripes)

	sleep 1

	# Now size consistency has to be fine
	[ $(_check_size $vg $lv $data_stripes) -eq 0 ] || die "LV size should be completely reduced"

	fsck -fy "$DM_DEV_DIR/$vg/$lv"

	udevadm settle
}

function _test
{
	local vg=$1
	local lv=$2
	local raid_type=$3
	local data_stripes=$4
	local cur_data_stripes

	_get_pvs_stripes_and_delay $raid_type

	aux prepare_pvs $npvs $pvsz
	get_devs

	vgcreate $SHARED -s 1M "$vg" "${DEVICES[@]}"

	# Calculate maximum rimage size in MiB and subtract 3 extents to leave room for rounding
	rimagesz=$(($(blockdev --getsz "$dev1") / 2048 - 3))

	# Create (data_stripes+1)-way striped $raid_type
	_lvcreate $raid_type $data_stripes $(($data_stripes * ${rimagesz}))M $vg $lv --stripesize 128k
	[ $(_check_size $vg $lv $data_stripes) -eq 0 ] || die "LV size bogus"
	check lv_first_seg_field $vg/$lv stripesize "128.00k"
	aux wait_for_sync $vg $lv 0

	# Delay its first 'PV' so that size can be evaluated before reshape finished too quick.
	aux delay_dev "$dev1" $ms 0

	# Reshape it to one more stripe and 256K stripe size
	_reshape_layout $raid_type $(($data_stripes + 1)) $vg $lv 0 0 --stripesize 256K
	[ $(_check_size $vg $lv $(($data_stripes + 1))) -ne 0 ] || die "LV size should still be small"
	fsck -fy "$DM_DEV_DIR/$vg/$lv"

	# Reset delay
	aux delay_dev "$dev1" 0 0

	# Wait for sync to finish to check frow extended LV size
	aux wait_for_sync $vg $lv 0

	[ $(_check_size $vg $lv $(($data_stripes + 1))) -eq 0 ] || die "LV size should be grown"
	fsck -fy "$DM_DEV_DIR/$vg/$lv"
	udevadm settle

	# Loop adding stripes and check size consistency on each iteration
	for data_stripes in $tst_stripes
	do
		cur_data_stripes=$(get lv_field "$vg/$lv" datastripes -a)
		[ $cur_data_stripes -lt $data_stripes ] && _add_stripes $raid_type $vg $lv $data_stripes \
							|| _remove_stripes $raid_type $vg $lv $data_stripes
	done

	vgremove -ff $vg
	aux remove_dm_devs "${DEVICES[@]}"
}

# Test all respective RAID levels
_test $vg $lv1 raid4 2
_test $vg $lv1 raid5_ra 2
_test $vg $lv1 raid6_nc 3
_test $vg $lv1 raid10 2
