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

# Check pvmove behavior when it's progress and machine is rebooted

. lib/test

aux prepare_pvs 3 60

vgcreate -s 128k $vg "$dev1" "$dev2" "$dev3"

# Create multisegment LV
lvcreate -an -Zn -l5 -n $lv1 $vg "$dev1"
lvextend -l+10 $vg/$lv1 "$dev2"
lvextend -l+5 $vg/$lv1 "$dev1"
lvextend -l+10 $vg/$lv1 "$dev2"

# Slowdown writes
aux delay_dev "$dev3" 0 100

lvs -o+devices $vg

pvmove -i0 -n $vg/$lv1 "$dev1" "$dev3" &
PVMOVE=$!
# Let's wait a bit till pvmove starts and kill it
sleep 1
kill -9 $PVMOVE

# Simulate reboot - forcibly remove related devices
dmsetup remove $vg-$lv1
dmsetup remove $vg-pvmove0

# Only PVs should be left in table...
dmsetup table

# Restart pvmove
vgchange -ay $vg

pvmove --abort

# Restore delayed device back
aux delay_dev "$dev3"

lvs -a -o+devices $vg
