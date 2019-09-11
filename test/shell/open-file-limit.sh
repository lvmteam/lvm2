#!/bin/bash
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test scan_lvs config setting

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

. lib/inittest

prlimit -h || skip

aux lvmconf 'devices/pv_min_size = 1024'

aux prepare_devs 200 1

for i in $(seq 1 200); do
	pvcreate "$DM_DEV_DIR/mapper/${PREFIX}pv$i"
done

pvs > out
test "$(grep pv out | wc -l)" -eq 200

# Set the soft limit to 100 fd's when 200 PVs need to be open.
# This requires lvm to increase its soft limit in order to
# process all the PVs.
# Test this with and without udev providing device lists.

aux lvmconf 'devices/obtain_device_list_from_udev = 0'

prlimit --nofile=100: pvs > out

test "$(grep pv out | wc -l)" -eq 200

aux lvmconf 'devices/obtain_device_list_from_udev = 1'

prlimit --nofile=100: pvs > out

test "$(grep pv out | wc -l)" -eq 200

