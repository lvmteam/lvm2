#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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

aux prepare_pvs 3

vgcreate -s 128k $vg  "$dev1" "$dev2"
vgcreate -s 128k $vg2 "$dev3"

lvcreate -V10M -L10M -T $vg/pool -n $lv1
lvcreate -L10M -n $lv2 $vg

lvchange -an $vg/$lv1

# Test activation
lvchange -aly $vg/$lv1
check active $vg $lv1

lvchange -aln $vg/$lv1
check inactive $vg $lv1

# Test for allowable changes
#
# contiguous_ARG
lvchange -C y $vg/$lv1
lvchange -C n $vg/$lv1

# permission_ARG
lvchange -p r $vg/$lv1
lvchange -p rw $vg/$lv1

# FIXME
#should lvchange -p r $vg/pool
#should lvchange -p rw $vg/pool

# readahead_ARG
lvchange -r none $vg/$lv1
lvchange -r auto $vg/$lv1
# FIXME
# Think about more support

# minor_ARG
lvchange --yes -M y --minor 234 --major 253 $vg/$lv1
lvchange -M n $vg/$lv1

# cannot change major minor for pools
not lvchange --yes -M y --minor 235 --major 253 $vg/pool
not lvchange -M n $vg/pool

# addtag_ARG
lvchange --addtag foo $vg/$lv1
lvchange --addtag foo $vg/pool

# deltag_ARG
lvchange --deltag foo $vg/$lv1
lvchange --deltag foo $vg/pool

# discards_ARG
lvchange --discards nopassdown $vg/pool
lvchange --discards passdown $vg/pool

# zero_ARG
lvchange --zero n $vg/pool
lvchange --zero y $vg/pool

#
# Test for disallowed metadata changes
#
# resync_ARG
not lvchange --resync $vg/$lv1

# alloc_ARG
#not lvchange --alloc anywhere $vg/$lv1

# discards_ARG
not lvchange --discards ignore $vg/$lv1

# zero_ARG
not lvchange --zero y $vg/$lv1


#
# Ensure that allowed args don't cause disallowed args to get through
#
not lvchange --resync -ay $vg/$lv1
not lvchange --resync --addtag foo $vg/$lv1

#
# Play with tags and activation
#
TAG=$(uname -n)
aux lvmconf "activation/volume_list = [ \"$vg/$lv2\", \"@mytag\" ]"

lvchange -ay $vg/$lv1
check inactive $vg $lv1

lvchange --addtag mytag $vg/$lv1

lvchange -ay @mytag_fake
check inactive $vg $lv1

lvchange -ay $vg/$lv1
# Volume has matching tag
check active $vg $lv1
lvchange -an $vg/$lv1

lvchange -ay @mytag
check active $vg $lv1

# Fails here since it cannot clear device header
not lvcreate -Zy -L10 -n $lv3 $vg2
# OK when zeroing is disabled
lvcreate -Zn -L10 -n $lv3 $vg2
check inactive $vg2 $lv3

aux lvmconf "activation/volume_list = [ \"$vg2\" ]"
vgchange -an $vg
vgchange -ay $vg $vg2
lvs -a -o+lv_active $vg $vg2

aux lvmconf "activation/volume_list = [ \"$vg\", \"$vg2\" ]"

vgremove -ff $vg $vg2
