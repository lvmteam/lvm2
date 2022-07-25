#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='duplicate pv detection of mpath components using wwid'

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

. lib/inittest

# FIXME: skip until mpath/scsi_debug cleanup works after a failure
skip

modprobe --dry-run scsi_debug || skip
multipath -l || skip
multipath -l | grep scsi_debug && skip

# FIXME: setting multipath_component_detection=0 now also disables
# the wwid-based mpath component detection, so this test will need
# to find another way to disable only the filter-mpath code (using
# sysfs and multipath/wwids) while keeping the code enabled that
# eliminates duplicates based on their matching wwids which this
# tries to test.

# Prevent wwids from being used for filtering.
aux lvmconf 'devices/multipath_wwids_file = "/dev/null"'
# Need to use /dev/mapper/mpath
aux lvmconf 'devices/dir = "/dev"'
aux lvmconf 'devices/scan = "/dev"'
# Could set filter to $MP and the component /dev/sd devs
aux lvmconf "devices/filter = [ \"a|.*|\" ]"
aux lvmconf "devices/global_filter = [ \"a|.*|\" ]"

modprobe scsi_debug dev_size_mb=100 num_tgts=1 vpd_use_hostno=0 add_host=4 delay=20 max_luns=2 no_lun_0=1
sleep 2

multipath -r
sleep 2

MPB=$(multipath -l | grep scsi_debug | cut -f1 -d ' ')
echo $MPB
MP=/dev/mapper/$MPB
echo $MP

pvcreate $MP
vgcreate $vg1 $MP
lvcreate -l1 $vg1
vgchange -an $vg1

pvs |tee out
grep $MP out
for i in $(grep -H scsi_debug /sys/block/sd*/device/model | cut -f4 -d /); do
	not grep /dev/$i out;
done

vgchange -an $vg1
vgremove -y $vg1

sleep 2
multipath -f $MP
sleep 1
rmmod scsi_debug
