#!/usr/bin/env bash

# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
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

which sfdisk || skip

aux prepare_devs 1 4

pvcreate_on_dev_with_part_table() {
	local dev=$1
	local type=$2

	# pvcreate passes on empty partition table
	echo "label:$type" | sfdisk "$dev"
	pvcreate -y "$dev"
	pvremove "$dev"

	# pvcreate fails if there's at least 1 partition
	echo "label:$type" | sfdisk "$dev"
	echo "1MiB 1" | sfdisk "$dev"
	not pvcreate "$dev" 2>err
	grep "device is partitioned" err

	aux wipefs_a "$dev"
}

pvcreate_on_dev_with_part_table "$dev1" "dos"
pvcreate_on_dev_with_part_table "$dev1" "gpt"
