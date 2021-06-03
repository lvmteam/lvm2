#!/usr/bin/env bash

# Copyright (C) 2021 Seagate, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# This testing script is for multi-hosts testing.
#
# On the host A:
#   make check_lvmlockd_idm \
#     LVM_TEST_BACKING_DEVICE=/dev/sdj3,/dev/sdk3,/dev/sdl3 \
#     LVM_TEST_MULTI_HOST=1 T=multi_hosts_lv_ex_timeout_hosta.sh
# On the host B:
#   make check_lvmlockd_idm \
#     LVM_TEST_BACKING_DEVICE=/dev/sdj3,/dev/sdk3,/dev/sdl3 \
#     LVM_TEST_MULTI_HOST=1 T=multi_hosts_lv_ex_timeout_hostb.sh

SKIP_WITH_LVMPOLLD=1

. lib/inittest

[ -z "$LVM_TEST_MULTI_HOST" ] && skip;

IFS=',' read -r -a BLKDEVS <<< "$LVM_TEST_BACKING_DEVICE"

for d in "${BLKDEVS[@]}"; do
	aux extend_filter_LVMTEST "a|$d|"
done

aux lvmconf "devices/allow_changes_with_duplicate_pvs = 1"

for d in "${BLKDEVS[@]}"; do
	dd if=/dev/zero of="$d" bs=32k count=1
	wipefs -a "$d" 2>/dev/null || true

	sg_dev=`sg_map26 ${d}`
	if [ -n "$LVM_TEST_LOCK_TYPE_IDM" ]; then
		echo "Cleanup IDM context for drive ${d} ($sg_dev)"
		sg_raw -v -r 512 -o /tmp/idm_tmp_data.bin $sg_dev \
			88 00 01 00 00 00 00 20 FF 01 00 00 00 01 00 00
		sg_raw -v -s 512 -i /tmp/idm_tmp_data.bin $sg_dev \
			8E 00 FF 00 00 00 00 00 00 00 00 00 00 01 00 00
		rm /tmp/idm_tmp_data.bin
	fi
done

for i in $(seq 1 ${#BLKDEVS[@]}); do
	vgcreate $SHARED TESTVG$i ${BLKDEVS[$(( i - 1 ))]}
	lvcreate -a n --zero n -l 1 -n foo TESTVG$i
	lvchange -a ey TESTVG$i/foo
done

for d in "${BLKDEVS[@]}"; do
	drive_wwn=`udevadm info $d | awk -F= '/E: ID_WWN=/ {print $2}'`
	for dev in /dev/*; do
		if [ -b "$dev" ] && [[ ! "$dev" =~ [0-9] ]]; then
			wwn=`udevadm info "${dev}" | awk -F= '/E: ID_WWN=/ {print $2}'`
			if [ "$wwn" = "$drive_wwn" ]; then
				base_name="$(basename -- ${dev})"
				drive_list+=("$base_name")
				host_list+=(`readlink /sys/block/$base_name | awk -F'/' '{print $6}'`)
			fi
		fi
	done
done

for d in "${drive_list[@]}"; do
	[ -f /sys/block/$d/device/delete ] && echo 1 > /sys/block/$d/device/delete
done

sleep 100

for i in $(seq 1 ${#BLKDEVS[@]}); do
	check grep_lvmlockd_dump "S lvm_TESTVG$i kill_vg"
	lvmlockctl --drop TESTVG$i
done

# Rescan drives so can probe the deleted drives and join back them
for h in "${host_list[@]}"; do
	[ -f /sys/class/scsi_host/${h}/scan ] && echo "- - -" > /sys/class/scsi_host/${h}/scan
done
