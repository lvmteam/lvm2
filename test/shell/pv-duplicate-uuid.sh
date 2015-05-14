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

# Test 'Found duplicate' is shown
. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux prepare_devs 3

pvcreate "$dev1"
UUID1=$(get pv_field "$dev1" uuid)
pvcreate --config "devices{filter=[\"a|$dev2|\",\"r|.*|\"]}" -u "$UUID1" --norestorefile "$dev2"
pvcreate --config "devices{filter=[\"a|$dev3|\",\"r|.*|\"]}" -u "$UUID1" --norestorefile "$dev3"

pvs -o+uuid 2>&1 | tee out
COUNT=$(should grep --count "Found duplicate" out)

# FIXME  lvmetad is not able to serve properly this case
should [ "$COUNT" -eq 2 ]

pvs -o+uuid --config "devices{filter=[\"a|$dev2|\",\"r|.*|\"]}"
