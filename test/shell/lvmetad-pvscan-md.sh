#!/bin/sh
# Copyright (C) 2014-2015 Red Hat, Inc. All rights reserved.
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

test -f /proc/mdstat && grep -q raid0 /proc/mdstat || \
	modprobe raid0 || skip

aux prepare_devs 2

# create 2 disk MD raid0 array (stripe_width=128K)
aux prepare_md_dev 0 64 2 "$dev1" "$dev2"

aux lvmconf 'devices/md_component_detection = 1'
aux extend_filter_LVMTEST
aux extend_filter "a|/dev/md.*|"

pvdev=$(< MD_DEV_PV)

pvcreate "$pvdev"

# ensure that lvmetad can only see the toplevel MD device
pvs | tee out
grep "$pvdev" out
not grep "$dev1" out
not grep "$dev2" out
