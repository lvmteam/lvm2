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

vgchange --lock-start

vgdisplay

for i in $(seq 1 ${#BLKDEVS[@]}); do
	not lvchange -a ey TESTVG$i/foo
done

# Sleep for 70 seconds so the previous lease is expired
sleep 70

for i in $(seq 1 ${#BLKDEVS[@]}); do
	lvchange -a ey TESTVG$i/foo
	lvchange -a n TESTVG$i/foo
done

for i in $(seq 1 ${#BLKDEVS[@]}); do
	vgremove -f TESTVG$i
done
