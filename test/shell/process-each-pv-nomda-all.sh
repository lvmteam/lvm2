#!/bin/sh
# Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Test process_each_pv with zero mda'

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux prepare_devs 14

# for vg1
pvcreate "$dev10"

# for vg2
pvcreate "$dev2" --metadatacopies 0
pvcreate "$dev3"
pvcreate "$dev4"
pvcreate "$dev5"

# for vg3
pvcreate "$dev6" --metadatacopies 0
pvcreate "$dev7" --metadatacopies 0
pvcreate "$dev8" --metadatacopies 0
pvcreate "$dev9"

# orphan with mda
pvcreate "$dev11"
# orphan without mda
pvcreate "$dev14" --metadatacopies 0

# non-pv devs
# dev12
# dev13

vgcreate $vg1 "$dev10"
vgcreate $vg2 "$dev2" "$dev3" "$dev4" "$dev5"
vgcreate $vg3 "$dev6" "$dev7" "$dev8" "$dev9"

pvs -a | tee err
grep "$dev10" err
grep "$dev2" err
grep "$dev3" err
grep "$dev4" err
grep "$dev5" err
grep "$dev6" err
grep "$dev7" err
grep "$dev8" err
grep "$dev9" err
grep "$dev11" err
grep "$dev12" err
grep "$dev13" err
grep "$dev14" err
