#!/bin/sh

# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
# test thin profile functionality
#

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

DEV_SIZE=32

# check we have thinp support compiled in
aux have_thin 1 0 0 || skip

aux prepare_profiles "thin-performance"

# Create scsi debug dev with sector size of 4096B and 1MiB optimal_io_size
aux prepare_scsi_debug_dev $DEV_SIZE sector_size=4096 opt_blks=256 || skip
EXPECT=1048576
check sysfs "$(< SCSI_DEBUG_DEV)" queue/optimal_io_size "$EXPECT"
aux prepare_pvs 1 $DEV_SIZE

# Check we are not running on buggy kernel (broken lcm())
# If so, turn chunk_size test into  'should'
check sysfs "$dev1" queue/optimal_io_size "$EXPECT" || SHOULD=should

vgcreate $vg "$dev1"

# By default, "generic" policy is used to
# calculate chunk size which is 64KiB by default
# or minimum_io_size if it's higher. Also, zeroing is used
# under default operation.
lvcreate -L8m -T $vg/pool_generic
check lv_field $vg/pool_generic profile ""
check lv_field $vg/pool_generic chunk_size 64.00k
check lv_field $vg/pool_generic zero "zero"

# If "thin-performance" profile is used, the "performance"
# policy is used to calculate chunk size which is 512KiB
# or optimal_io_suize if it's higher. Our test device has
# 1MiB, so that should be used. Also, zeroing is not used
# under "thin-perforance" profile.
lvcreate --profile thin-performance -L8m -T $vg/pool_performance
check lv_field $vg/pool_performance profile "thin-performance"
$SHOULD check lv_field $vg/pool_performance chunk_size 1.00m
check lv_field $vg/pool_performance zero ""

vgremove -ff $vg

# The profile must be also applied if using the profile
# for the whole VG - any LVs inherit this profile then.
vgcreate --profile thin-performance $vg "$dev1"
lvcreate -L8m -T $vg/pool_performance_inherited
# ...the LV does not have the profile attached, but VG does!
check vg_field $vg profile "thin-performance"
check lv_field $vg/pool_performance_inherited profile ""
$SHOULD check lv_field $vg/pool_performance_inherited chunk_size 1.00m
check lv_field $vg/pool_performance_inherited zero ""
