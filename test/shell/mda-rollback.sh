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

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux prepare_devs 3

vgcreate --metadatasize 128k $vg1 "$dev1" "$dev2" "$dev3"

vgreduce $vg1 "$dev1"
dd if="$dev1" of=badmda bs=256K count=1
vgextend $vg1 "$dev1"

dd if=badmda of="$dev1" bs=256K count=1

# dev1 is part of vg1 (as witnessed by metadata on dev2 and dev3), but its mda
# was corrupt (written over by a backup from time dev1 was an orphan)
check pv_field "$dev1" vg_name $vg1
