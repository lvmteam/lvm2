#!/usr/bin/env bash

# Copyright (C) 2024,2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA2110-1301 USA
#

#
# Due to MD constraints, three issues must be considered when modifying stripe counts that alter the RAID-mapped device size:
# - on a stripe adding reshape, the initial size remains until after the end when the device capacity is increased
# - on a stripe removing reshape, the initial size is reduced before the reshape is allowed to start
# - size changes are being reported asynchronously by the kernel, which requires polling for it for split seconds
#

LVM_SKIP_LARGE_TESTS=${LVM_SKIP_LARGE_TESTS:-1}
SKIP_FSCK=${SKIP_FSCK:-1}
SKIP_RESIZE=${SKIP_RESIZE:-0}

# With the use of 'hack' --noudevsync we need to also make sure our device paths are
# present in the system, otherwise we will not be able to open created LVs
# since we are not wait for udev to create them.
# FIXME: it would be better to not need this
declare -a UDEVOPTS=( "--noudevsync" "--config" "activation/verify_udev_operations=1" )

# Timeout in seconds to check for size updates happening during/after.
CHECK_SIZE_TIMEOUT=5

# List of stripes to add or remove
tst_stripes=""

# Delay on first 'PV' to be able to retrieve status during reshape.
# Needs to be varied depending on layout as they come with different reshape overhead.
ms=0

# PV numbers and size
npvs=0
pvsz=0


. lib/inittest

test "${LVM_VALGRIND:-0}" -eq 0 || skip "Timing is too slow with valgrind."

aux kernel_at_least 6 9 0 || skip

test "$(aux total_mem)" -gt 1048576 || skip "Not enough RAM for this test"

which mkfs.ext4 || skip

test "$SKIP_RESIZE" -eq 1 || ( which resize2fs || skip )

# Delay its first 'PV' so that size can be evaluated before reshape finished too quick.
function _delay_dev
{
	local dev=$1
	local offset=$(( $(get first_extent_sector "$dev") + 2048 ))

	aux delay_dev "$dev" "$ms" 0 "$offset"
}

# Reset delay
function _restore_dev
{
	local dev=$1

	aux enable_dev "$dev"
}

# Optimized filesystem check function with better error handling
function _skip_or_fsck
{
	local device=$1
	local options=${2:-y}

	[ "$SKIP_FSCK" -eq 1 ] || fsck -f"$options" "$device"
}

function _get_pvs
{
	case "$LVM_SKIP_LARGE_TESTS" in
	0)
		npvs=64
		pvsz=32
		;;
	*)
		npvs=20
		pvsz=10
		;;
	esac
}

function _get_stripes_and_delay
{
	local raid_type=$1

	case "$LVM_SKIP_LARGE_TESTS" in
	0)
		case "$raid_type" in
		raid[45]*)
			ms=60
			tst_stripes="5 4 11 9 15 13 19 18 22 21 25 23 28 30 31 29 33 31 34 33 37 45 41 50 55 60 63 30 15 10 8 3"
			;;
		raid6*)
			ms=40
			tst_stripes="5 4 11 9 15 13 19 18 22 21 25 23 28 30 31 29 33 31 34 33 37 45 41 50 55 60 62 30 15 10 8 3"
			;;
		raid10*)
			ms=50
			tst_stripes="5 8 9 11 13 14 15 19 20 22 23 25 27 28 29 32"
			;;
		esac
		;;
	*)
		case "$raid_type" in
		raid[45]*)
			ms=60
			tst_stripes="5 4 8 7 11 9 15 19 18 10"
			;;
		raid6*)
			ms=40
			tst_stripes="5 4 8 7 11 9 15 18 10 5"
			;;
		raid10*)
			ms=50
			tst_stripes="5 6 7 8 9 10"
			;;
		esac
		;;
	esac
}

function _get_size
{
	local vg=$1
	local lv=$2
	local data_stripes=$3
	local reshape_len rimagesz

	# Get rimage size early
	rimagesz=$(blockdev --getsz "$DM_DEV_DIR/mapper/${vg}-${lv}_rimage_0")

	# Get any reshape size in sectors
	# Avoid using pipes as exit codes may cause test failure
	reshape_len=$(lvs --noheadings --nosuffix -aoreshapelen --unit s "$vg/${lv}_rimage_0")
	# Drop everything past 'S'
	reshape_len=${reshape_len/S*}

	# Get rimage size - reshape length
	rimagesz=$(( rimagesz - reshape_len ))

	# Calculate size of LV based on above sizes
	echo $(( rimagesz *  data_stripes ))
}

function _check_size
{
	local vg=$1
	local lv=$2
	local data_stripes=$3

	# Compare size of LV with calculated one
	test "$(blockdev --getsz "/dev/$vg/$lv")" -eq "$(_get_size $vg $lv "$data_stripes")"
}

function _check_size_timeout
{
	local i

	for i in $(seq 0 $((CHECK_SIZE_TIMEOUT * 20)))
	do
		_check_size "$@" && return
		sleep .05
	done

	return 1
}

function _total_stripes
{
	local raid_type=$1
	local data_stripes=$2

	case "$raid_type" in
	raid[45]*) echo $(( data_stripes + 1 )) ;;
	raid6*)    echo $(( data_stripes + 2 )) ;;
	raid10*)   echo $(( data_stripes * 2 )) ;;
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
	local stripes

	stripes=$(_total_stripes "$raid_type" "$data_stripes")

	lvcreate -y -aey --type "$raid_type" -i "$data_stripes" -L "$size" -n $lv $vg "$@"

	check lv_first_seg_field $vg/$lv segtype "$raid_type"
	check lv_first_seg_field $vg/$lv datastripes "$data_stripes"
	check lv_first_seg_field $vg/$lv stripes "$stripes"

	echo y|mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
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
	local stripes

	stripes=$(_total_stripes "$raid_type" "$data_stripes")

	# FIXME: replace this hack with --noudevsync with slowdown of 'write'
	#        areas used for reshape operation.
	#   ATM: command used to be 'sleeping' waiting for a cookie - delayed by udev
	lvconvert -y --ty "$raid_type" --stripes "$data_stripes" "${UDEVOPTS[@]}" $vg/$lv "$@"
	check lv_first_seg_field $vg/$lv1 segtype "$raid_type"

	if [ "$wait_for_reshape" -eq 1 ]
	then
		_restore_dev "$dev1"
		aux wait_for_sync $vg $lv "$ignore_a_chars"
		_skip_or_fsck "$DM_DEV_DIR/$vg/$lv" "fn"
	fi
}

function _add_stripes
{
	local raid_type=$1
	local vg=$2
	local lv=$3
	local data_stripes=$4
	local stripes=
	local stripesize="$((16 << (data_stripes % 5))).00k" # Stripe size variation

	stripes=$(_total_stripes "$raid_type" "$data_stripes")

	_delay_dev "$dev1"
	_reshape_layout "$raid_type" "$data_stripes" $vg $lv 0 1 --stripesize "$stripesize"

	# Size has to be inconsistent until reshape finishes
	not _check_size $vg $lv "$data_stripes" || die "LV size should be small"

	_restore_dev "$dev1"

	check lv_first_seg_field $vg/$lv stripesize "$stripesize"
	check lv_first_seg_field $vg/$lv datastripes "$data_stripes"
	check lv_first_seg_field $vg/$lv stripes "$stripes"

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
	aux wait_for_sync $vg $lv 0

	# Now size consistency has to be fine
	not _check_size_timeout $vg $lv "$data_stripes" || die "LV size should be grown"

	# Check, use grown capacity for the filesystem and check again
	if [ "$SKIP_RESIZE" -eq 0 ]
	then
		# Mandatory fsck before resize2fs.
		fsck -fy "$DM_DEV_DIR/$vg/$lv"
		resize2fs "$DM_DEV_DIR/$vg/$lv"
	fi

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
}

function _remove_stripes
{
	local raid_type=$1
	local vg=$2
	local lv=$3
	local data_stripes=$4
	local cur_data_stripes
	local stripes
	local stripesize="$((16 << (data_stripes % 5))).00k" # Stripe size variation

	cur_data_stripes=$(get lv_field "$vg/$lv" datastripes -a)
	stripes=$(get lv_field "$vg/$lv" stripes -a)

	# Check, shrink filesystem to the resulting smaller size and check again
	if [ "$SKIP_RESIZE" -eq 0 ]
	then
		# Mandatory fsck before resize2fs.
		fsck -fy "$DM_DEV_DIR/$vg/$lv"
		resize2fs "$DM_DEV_DIR/$vg/$lv" "$(_get_size $vg $lv "$data_stripes")s"
		_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
	fi

	_delay_dev "$dev1"
	_reshape_layout "$raid_type" "$data_stripes" $vg $lv 0 1 --force --stripesize "$stripesize"

	# Size has to be inconsistent, as to be removed legs still exist
	not _check_size $vg $lv "$cur_data_stripes" || die "LV size should be reduced but not rimage count"

	_restore_dev "$dev1"

	check lv_first_seg_field $vg/$lv stripesize "$stripesize"
	check lv_first_seg_field $vg/$lv datastripes "$data_stripes"
	check lv_first_seg_field $vg/$lv stripes "$stripes"

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"

	# Have to remove freed legs before another restriping conversion. Will fail while reshaping is ongoing as stripes are still in use
	not _reshape_layout "$raid_type" $(( data_stripes + 1 )) 0 1 $vg $lv --force
	aux wait_for_sync $vg $lv 1

	# Remove freed legs as they are now idle has to succeed without --force
	_reshape_layout "$raid_type" "$data_stripes" $vg $lv 1 1
	check lv_first_seg_field $vg/$lv datastripes "$data_stripes"
	check lv_first_seg_field $vg/$lv stripes "$(_total_stripes "$raid_type" "$data_stripes")"

	# Now size consistency has to be fine
	_check_size_timeout $vg $lv "$data_stripes" || die "LV size should be completely reduced"

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv" || return 0
}

function _test
{
	local vg=$1
	local lv=$2
	local raid_type=$3
	local data_stripes=$4
	local cur_data_stripes
	local rimagesz

	_get_stripes_and_delay "$raid_type"

	# Calculate maximum rimage size in MiB and subtract 3 extents to leave room for rounding
	rimagesz=$(( $(blockdev --getsz "$dev1") / 2048 - 3 ))

	# Create (data_stripes+1)-way striped $raid_type
	_lvcreate "$raid_type" "$data_stripes" $(( data_stripes * rimagesz ))M $vg $lv --stripesize 128k
	_check_size $vg $lv "$data_stripes" || die "LV size bogus"
	check lv_first_seg_field $vg/$lv stripesize "128.00k"
	aux wait_for_sync $vg $lv 0

	_delay_dev "$dev1"

	# Reshape it to one more stripe and 256K stripe size
	_reshape_layout "$raid_type" $(( data_stripes + 1 )) $vg $lv 0 0 --stripesize 256K
	_check_size $vg $lv "$data_stripes" || die "LV size should still be small"

	_restore_dev "$dev1"
	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"

	# Wait for sync to finish to check frow extended LV size
	aux wait_for_sync $vg $lv 0

	_check_size_timeout $vg $lv $(( data_stripes + 1 )) || die "LV size should be grown"
	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"

	# Loop adding stripes and check size consistency on each iteration
	for data_stripes in $tst_stripes
	do
		cur_data_stripes=$(get lv_field "$vg/$lv" datastripes -a)
		if [ "$cur_data_stripes" -lt "$data_stripes" ]; then
			_add_stripes "$raid_type" $vg $lv "$data_stripes"
		else
			_remove_stripes "$raid_type" $vg $lv "$data_stripes"
		fi
	done

	lvremove -ff $vg
}

#
# Main
#
_get_pvs
aux prepare_pvs $npvs $pvsz
get_devs
vgcreate $SHARED -s 1M "$vg" "${DEVICES[@]}"

# Test all respective RAID levels
_test $vg $lv1 raid4 2
_test $vg $lv1 raid5_ra 2
_test $vg $lv1 raid6_nc 3
_test $vg $lv1 raid10 2

vgremove -ff $vg
