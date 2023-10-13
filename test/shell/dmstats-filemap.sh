#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

. lib/inittest

which mkfs.xfs || skip

# Don't attempt to test stats with driver < 4.33.00
aux driver_at_least 4 33 || skip

# ensure we can create devices (uses dmsetup, etc)
aux prepare_devs 1 2048

mount_dir="mnt"
hist_bounds="10ms,20ms,30ms"
file_size="100m"

test ! -d "$mount_dir" && mkdir "$mount_dir"

cleanup_mounted_and_teardown()
{
	umount "$mount_dir" 2>/dev/null || true
	aux teardown
}

trap 'cleanup_mounted_and_teardown' EXIT

mkfs.xfs "$dev1"
mount "$dev1" "$mount_dir"

test_filemap()
{
	local map_file="$1"
	local use_precise="$2"
	local use_bounds="$3"
	if [[ $use_precise == 1 ]]; then
		precise="--precise"
	else
		precise=""
	fi
	if [[ $use_bounds == 1 ]]; then
		bounds="--bounds $hist_bounds"
	else
		bounds=""
	fi
	fallocate -l "$file_size" "$mount_dir/$map_file"
	dmstats create ${precise} ${bounds} --filemap "$mount_dir/$map_file"
	dmstats list -ostats_name,precise |& tee out
	grep "$map_file" out
	if [[ $use_precise == 1 ]]; then
		grep "1" out
	fi
	if [[ $use_bounds == 1 ]]; then
		dmstats list -ostats_name,hist_bounds |& tee out
		grep "$hist_bounds" out
	fi
	dmstats delete --allregions --alldevices
	rm -f "$mount_dir/$map_file"
}

for file in "filenospace" "File With Spaces" "$(echo -ne 'file\twith\ttabs')"; do
	for precise in 0 1; do
		for bounds in 0 1; do
			test_filemap "$file" "$precise" "$bounds"
		done
	done
done

sleep .5
