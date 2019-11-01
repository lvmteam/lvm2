#!/usr/bin/env bash

# Copyright (C) 2019 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

. lib/inittest

LOGICAL_BLOCK_SIZE=4096

# PHYSICAL_BLOCK_SIZE is set with physblk_exp which
# shifts the logical block size value.

# 4096 << 9 = 2MB physical block size
PHYSICAL_BLOCK_SHIFT=9

aux prepare_scsi_debug_dev 256 sector_size=$LOGICAL_BLOCK_SIZE physblk_exp=$PHYSICAL_BLOCK_SHIFT || skip

check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "$LOGICAL_BLOCK_SIZE"

aux prepare_pvs 1 256

get_devs

vgcreate $SHARED $vg "$dev1"

for i in `seq 1 40`; do lvcreate -an -l1 $vg; done;

lvs $vg

lvremove -y $vg

vgremove $vg

aux cleanup_scsi_debug_dev
