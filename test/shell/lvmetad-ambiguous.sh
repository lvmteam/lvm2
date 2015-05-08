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

aux prepare_pvs 2

# flip the devices around
aux init_udev_transaction
dmsetup remove -f "$dev1"
dmsetup remove -f "$dev2"
dmsetup create -u TEST-${PREFIX}pv2 ${PREFIX}pv2 ${PREFIX}pv2.table
dmsetup create -u TEST-${PREFIX}pv1 ${PREFIX}pv1 ${PREFIX}pv1.table
aux finish_udev_transaction

# re-scan them
pvscan --cache "$dev1"
pvscan --cache "$dev2"

# expect both to be there
pvs | tee pvs.txt
grep "$dev1" pvs.txt
grep "$dev2" pvs.txt

