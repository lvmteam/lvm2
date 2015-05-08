#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/inittest

test -e LOCAL_LVMETAD || skip
test -e LOCAL_LVMPOLLD && skip

aux prepare_devs 2
pvcreate --metadatatype 1 "$dev1"
should vgscan --cache
pvs | should grep "$dev1"
vgcreate --metadatatype 1 $vg1 "$dev1"
should vgscan --cache
vgs | should grep $vg1
pvs | should grep "$dev1"

# check for RHBZ 1080189 -- SEGV in lvremove/vgremove
pvcreate -ff -y --metadatatype 1 "$dev1" "$dev2"
vgcreate --metadatatype 1 $vg1 "$dev1" "$dev2"
lvcreate -l1 $vg1
pvremove -ff -y "$dev2"
vgchange -an $vg1
not lvremove $vg1
not vgremove -ff -y $vg1
