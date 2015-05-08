#!/bin/sh
# Copyright (C) 2007-2014 Red Hat, Inc. All rights reserved.
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

aux prepare_vg 2 80

lvcreate -L 10M -n lv -i2 $vg
lvresize -l +4 $vg/lv
lvremove -ff $vg

lvcreate -L 64M -n $lv -i2 $vg
not lvresize -v -l +4 xxx/$lv

# Check stripe size is reduced to extent size when it's bigger
ESIZE=$(get vg_field $vg vg_extent_size --units b)
lvextend -L+64m -i 2 -I$(( ${ESIZE%%B} * 2 ))B $vg/$lv 2>&1 | tee err
grep "Reducing stripe size" err

lvremove -ff $vg

lvcreate -L 10M -n lv $vg "$dev1"
lvextend -L +10M $vg/lv "$dev2"

# Attempt to reduce with lvextend and vice versa:
not lvextend -L 16M $vg/lv
not lvreduce -L 32M $vg/lv

lvremove -ff $vg
