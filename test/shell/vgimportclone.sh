#!/bin/sh
# Copyright (C) 2010-2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux prepare_devs 2

vgcreate --metadatasize 128k $vg1 "$dev1"
lvcreate -l100%FREE -n $lv1 $vg1

# Clone the LUN
dd if="$dev1" of="$dev2" bs=256K count=1
aux notify_lvmetad "$dev2"

# Verify pvs works on each device to give us vgname
aux hide_dev $dev2
check pv_field "$dev1" vg_name $vg1
aux unhide_dev $dev2

aux hide_dev $dev1
check pv_field "$dev2" vg_name $vg1
aux unhide_dev $dev1

# Import the cloned PV to a new VG
vgimportclone --basevgname $vg2 "$dev2"

# We need to re-scan *both* $dev1 and $dev2 since a PV, as far as lvmetad is
# concerned, can only live on a single device. With the last pvscan, we told it
# that PV from $dev1 now lives on $dev2, but in fact this is not true anymore,
# since we wrote a different PV over $dev2.
rm -f $TESTDIR/etc/.cache
aux notify_lvmetad "$dev2"
aux notify_lvmetad "$dev1"

# Verify we can activate / deactivate the LV from both VGs
lvchange -ay $vg1/$lv1 $vg2/$lv1
vgchange -an $vg1 $vg2
