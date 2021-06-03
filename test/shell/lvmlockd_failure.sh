#!/usr/bin/env bash

# Copyright (C) 2020~2021 Seagate, Inc.  All rights reserved.
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

[ -z "$LVM_TEST_FAILURE" ] && skip;

aux prepare_vg 3

# Create new logic volume
lvcreate -a ey --zero n -l 1 -n $lv1 $vg

# Emulate lvmlockd abnormally exiting
killall -9 lvmlockd

systemctl start lvm2-lvmlockd

vgchange --lock-start $vg

lvchange -a n $vg/$lv1
lvchange -a sy $vg/$lv1

lvcreate -a ey --zero n -l 1 -n $lv2 $vg
lvchange -a n $vg/$lv2

vgremove -ff $vg
