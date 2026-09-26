#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Check pvmove behaviour when its device are kept open


. lib/inittest --skip-with-lvmlockd

_create_lv()
{
	lvremove -f $vg
	lvcreate -Zn -L8 -n $lv1 $vg "$dev1"
	rm -f debug.log_DEBUG*
}

_wait_until() {
	local die_msg=$1
	local fn=$2
	shift 2
	local i

	for i in {30..0}; do
		test "$i" -eq 0 && die "$die_msg"
		$fn "$@" && break
		sleep .1 || return 0
	done
}

_open_mapper_dev() {
	local dev=$1

	# try to keep device open for a while
	exec 2>/dev/null {fd}<"$dev"
}

# A fast machine can finish the copy before we manage to open the pvmove
# device, so writes to $dev3 are slowed first.  _keep_open_wait runs in the
# background holding the named devices open and restores full speed on $dev3
# once they are all held.
_keep_open()
{
	aux delay_dev "$dev3" 0 2 "$(get first_extent_sector "$dev3"):"

	_keep_open_wait "$@" &
	KEEP_OPEN_PID=$!
}

_keep_open_wait()
{
	export LVM_TEST_TAG="kill_me_$PREFIX"
	local name dev

	for name in "$@"; do
		dev="$DM_DEV_DIR/mapper/$vg-$name"
		_wait_until "Failed to wait and open: $dev!" _open_mapper_dev "$dev"
		echo "Keeping open: $dev."
	done

	aux enable_dev "$dev3"
	sleep 10 || true
}

_check_msg()
{
	# skipping this check for test with lvmpolld
	# we would need to check 'lvmpolld' messages to get this message
	test -e LOCAL_LVMPOLLD && return

	grep "$@"
}

aux target_at_least dm-mirror 1 2 0 || skip

aux prepare_vg 3

# do not waste 'testing' time on 'retry deactivation' loops
aux lvmconf 'activation/retry_deactivation = 0' \
	    'activation/raid_region_size = 16'

# fallback to mirror throttling when dm-delay is not available
# this does not work too well with fast CPUs
aux target_at_least dm-delay 1 1 0 || { aux throttle_dm_mirror || skip ; }

########################################################
# pvmove operation finishes, while 1 mirror leg is open
########################################################

_create_lv
_keep_open pvmove0_mimage_0

# pvmove fails in such case
not pvmove -i0 --atomic "$dev1" "$dev3" -vvvv |& tee out

aux kill_tagged_processes
kill "$KEEP_OPEN_PID" 2>/dev/null || true
wait "$KEEP_OPEN_PID" || true

_check_msg "ABORTING: Failed" out

lvs -ao+seg_pe_ranges $vg
# but LVs were already moved
check lv_on $vg $lv1 "$dev3"
lvs -a $vg

# orphan LV should be visible with error segment and removable
check lv_field $vg/pvmove0_mimage_0 layout "error"
check lv_field $vg/pvmove0_mimage_0 role "public"
lvremove -f $vg/pvmove0_mimage_0


##########################################
# abort pvmove while 1 mirror leg is open
##########################################

_create_lv
# Capture _keep_open stdout to detect when it actually holds the device open.
# Checking dmsetup open_count is not sufficient -- it may see transient opens
# from the pvmove polling process before _keep_open has opened the device.
_keep_open pvmove0_mimage_1 >keep_open_log 2>&1

# '-i +N' makes the polling process wait N seconds before its first copy
# status check.  That check is what runs finish_copy and removes pvmove0
# from the metadata, so with '+N' the pvmove cannot complete before that
# check no matter how fast the machine copies the data.  The abort below
# needs pvmove0 to still be present, so this is the window it relies on;
# unlike a device delay it does not depend on how fast the copy runs.
# Aborting skips the wait and kills the poller, so the sleep costs nothing.
LVM_TEST_TAG="kill_me_$PREFIX" \
	pvmove -b -i +10 --atomic -vvvv "$dev1" "$dev3"
aux wait_pvmove_lv_ready "$vg-pvmove0"
# Wait until _keep_open prints "Keeping open", confirming the device is held.
_wait_until "Timed out waiting for _keep_open to open pvmove0_mimage_1" \
	grep -q "Keeping open" keep_open_log 2>/dev/null

not pvmove -i0 --abort -vvvv |& tee out

aux kill_tagged_processes
wait "$KEEP_OPEN_PID" || true
# With '+N' a forked poller sleeps instead of noticing the abort within one
# polling interval, so it is tagged above and reaped by kill_tagged_processes
# instead of being waited out with a fixed sleep.
if pgrep lvmdbusd; then
        echo "Skipping check for lvm processes, since lvmdbusd is running!"
else
	pgrep -u root -ax lvm >out_pgrep || true
	not grep "$PREFIX" out_pgrep || {
		ps aux
		die "Some 'lvm' process of this test keeps running!"
	}
fi

_check_msg "ABORTING: Failed" out

# hopefully we managed to abort before pvmove finished
check lv_on $vg $lv1 "$dev1"

check lv_field $vg/pvmove0_mimage_1 layout "error"
check lv_field $vg/pvmove0_mimage_1 role "public"

lvremove -f $vg/pvmove0_mimage_1


#############################################
# keep pvmove0 open while it tries to finish
#############################################

_create_lv
_keep_open pvmove0

not pvmove -i0 --atomic "$dev1" "$dev3" |& tee out

aux kill_tagged_processes
wait "$KEEP_OPEN_PID" || true

_check_msg "ABORTING: Unable to deactivate" out

check lv_field $vg/pvmove0 layout "error"
check lv_field $vg/pvmove0 role "public"
lvremove -f $vg/pvmove0


################################################
# keep all pvmove volumes open
################################################

_create_lv
_keep_open pvmove0_mimage_0 pvmove0_mimage_1 pvmove0

not pvmove -i0 --atomic -vvvv "$dev1" "$dev3" |& tee out

aux kill_tagged_processes
wait "$KEEP_OPEN_PID" || true

_check_msg "ABORTING: Unable to deactivate" out

lvremove -f $vg/pvmove0_mimage_0
lvremove -f $vg/pvmove0_mimage_1
lvremove -f $vg/pvmove0


# Restore throttling
aux restore_dm_mirror

vgremove -ff $vg
