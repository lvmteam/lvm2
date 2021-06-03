#!/usr/bin/env bash

# Copyright (C) 2020 Seagate, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# This testing script is for multi-hosts testing, the paired scripts
# are: multi_hosts_lv_hosta.sh / multi_hosts_lv_hostb.sh
#
# On the host A:
#   make check_lvmlockd_idm \
#     LVM_TEST_BACKING_DEVICE=/dev/sdj3,/dev/sdk3,/dev/sdl3 \
#     LVM_TEST_MULTI_HOST=1 T=multi_hosts_lv_hosta.sh
# On the host B:
#   make check_lvmlockd_idm \
#     LVM_TEST_BACKING_DEVICE=/dev/sdj3,/dev/sdk3,/dev/sdl3 \
#     LVM_TEST_MULTI_HOST=1 T=multi_hosts_lv_hostb.sh

SKIP_WITH_LVMPOLLD=1

. lib/inittest

[ -z "$LVM_TEST_MULTI_HOST" ] && skip;

IFS=',' read -r -a BLKDEVS <<< "$LVM_TEST_BACKING_DEVICE"

for d in "${BLKDEVS[@]}"; do
	aux extend_filter_LVMTEST "a|$d|"
done

aux lvmconf "devices/allow_changes_with_duplicate_pvs = 1"

BLKDEVS_NUM=${#BLKDEVS[@]}

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

#aux prepare_pvs $BLKDEVS_NUM 6400

for i in $(seq 1 ${#BLKDEVS[@]}); do
	echo $i
	d="dev$i"
	vgcreate $SHARED TESTVG$i ${BLKDEVS[$(( i - 1 ))]}

	for j in {1..20}; do
		lvcreate -a n --zero n -l 1 -n foo$j TESTVG$i
	done
done

for i in $(seq 1 ${#BLKDEVS[@]}); do
	for j in {1..20}; do
		lvchange -a ey TESTVG$i/foo$j
	done
done

for i in $(seq 1 ${#BLKDEVS[@]}); do
	for j in {1..20}; do
		lvchange -a n TESTVG$i/foo$j
	done
done
