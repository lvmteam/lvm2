#!/bin/sh
# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

#
# Test to exercise larger number of PVs in a VG
# Related to https://bugzilla.redhat.com/show_bug.cgi?id=736027
# 
# Original measured times of the whole test case before 
# and with the acceleration patch from my bare metal hw
# (Lenovo T61, 2.2GHz, 4G RAM, rawhide 2015-03-06 with ndebug kernel):
#
# export LVM_TEST_PVS=300
#
# make check_local   ~52sec  (U:29s, S:13s)
# make check_lvmetad ~20sec  (U: 4s, S: 5s)
#
# With patch from 2015-03-06:
#
# make check_local   ~30sec  (U:10s, S:12s)
# make check_lvmetad ~20sec  (U: 4s, S: 5s)
#

# TODO: extend test suite to monitor performance and report regressions...

# Use just 100 to get 'decent' speed on slow boxes
LVM_TEST_PVS=${LVM_TEST_PVS:-100}

#aux prepare_devs $LVM_TEST_PVS 8
#vgcreate $vg $(< DEVICES)

# prepare_vg is now directly using steps above
aux prepare_vg $LVM_TEST_PVS

# Check we have decent speed with typical commands
vgs

lvs

pvs

lvcreate -l1 -n $lv1 $vg

lvremove -f $vg/$lv1

vgremove -ff $vg

# 
# TODO Turn this into another test case:
#
#for i in $(seq 1 $LVM_TEST_PVS); do
#	vgcreate ${vg}$i "$DM_DEV_DIR/mapper/${PREFIX}pv$i"
#done
