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

. lib/inittest

test -e LOCAL_LVMETAD || skip
test -e LOCAL_LVMPOLLD && skip

aux prepare_pvs 2

vgcreate $vg1 "$dev1" "$dev2"
lvcreate -n testlv --type mirror -m 1 -l 1 $vg1
vgs | grep $vg1

lvscan --cache $vg1/testlv

vgs | grep $vg1

aux disable_dev "$dev2"

# pvscan --cache already ran for the disabled device above, this should be a
# no-op (but should not segfault!)
lvscan --cache $vg1/testlv

vgremove -ff $vg1
