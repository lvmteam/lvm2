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

vgcreate $vg1 "$dev1" "$dev2"
vgs | grep $vg1

pvscan --cache

vgs | grep $vg1

# When MDA is ignored on PV, do not read any VG
# metadata from such PV as it may contain old
# metadata which hasn't been updated for some
# time and also since the MDA is marked as ignored,
# it should really be *ignored*!
pvchange --metadataignore y "$dev1"
aux disable_dev "$dev2"
pvscan --cache
check pv_field "$dev1" vg_name ""
aux enable_dev "$dev2"
pvscan --cache
check pv_field "$dev1" vg_name "$vg1"

vgremove -ff $vg1
