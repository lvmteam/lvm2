#!/bin/bash
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Basic usage of zero target

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

which md5sum || skip

aux prepare_pvs 1

vgcreate -s 256k $vg $(cat DEVICES)

lvcreate --type zero -L1 -n $lv1 $vg
lvextend -L+1 $vg/$lv1

sum1=$(dd if=/dev/zero bs=2M count=1 | md5sum | cut -f1 -d' ')
sum2=$(dd if="$DM_DEV_DIR/$vg/$lv1" bs=2M count=1 | md5sum | cut -f1 -d' ')

# has to match
test "$sum1" = "$sum2"

check lv_field $vg/$lv1 lv_modules "zero"
check lv_field $vg/$lv1 segtype "zero"
check lv_field $vg/$lv1 seg_count "1"
check lv_field $vg/$lv1 seg_size_pe "8"   # 8 * 256
