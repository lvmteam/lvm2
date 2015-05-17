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

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux have_thin 1 0 0 || skip

aux prepare_vg 3
lvcreate -i2 -l2 -T $vg/pool2
lvextend -l+2 $vg/pool2 "$dev2" "$dev3"
should lvextend -l+100%FREE $vg/pool2
