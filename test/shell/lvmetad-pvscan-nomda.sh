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

. lib/test

test -e LOCAL_LVMETAD || skip
kill $(cat LOCAL_LVMETAD)
rm LOCAL_LVMETAD

aux prepare_devs 2

pvcreate --metadatacopies 0 $dev1
pvcreate --metadatacopies 1 $dev2
vgcreate $vg1 $dev1 $dev2
lvcreate -n foo -l 1 -an --zero n $vg1

# start lvmetad but make sure it doesn't know about $dev1 or $dev2
aux disable_dev $dev1
aux disable_dev $dev2
aux prepare_lvmetad
lvs
mv LOCAL_LVMETAD XXX
aux enable_dev $dev2
aux enable_dev $dev1
mv XXX LOCAL_LVMETAD

aux lvmconf 'global/use_lvmetad = 0'
check inactive $vg1 foo
aux lvmconf 'global/use_lvmetad = 1'

pvscan --cache $dev2 -aay

aux lvmconf 'global/use_lvmetad = 0'
check inactive $vg1 foo
aux lvmconf 'global/use_lvmetad = 1'

pvscan --cache $dev1 -aay

aux lvmconf 'global/use_lvmetad = 0'
check active $vg1 foo
aux lvmconf 'global/use_lvmetad = 1'
