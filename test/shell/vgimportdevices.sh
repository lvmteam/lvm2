#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='vgimportdevices'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 5

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir "$DFDIR" || true
DF="$DFDIR/system.devices"

aux lvmconf 'devices/use_devicesfile = 1'

not ls "$DF"
pvcreate "$dev1"
ls "$DF"
grep "$dev1" "$DF"
rm -f "$DF"
dd if=/dev/zero of="$dev1" bs=1M count=1

#
# vgimportdevices -a with no prev df
#

# no devs found
not vgimportdevices -a
not ls "$DF"

# one orphan pv, no vgs
pvcreate "$dev1"
rm -f "$DF"
not vgimportdevices -a
not ls "$DF"

# one complete vg
vgcreate $vg1 "$dev1"
rm -f "$DF"
vgimportdevices -a
ls "$DF"
grep "$dev1" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
rm -f "$DF"

# two complete vgs
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
rm -f "$DF"
vgimportdevices -a
ls "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
rm -f "$DF"

# one incomplete vg
vgcreate $vg1 "$dev1" "$dev2"
lvcreate -l1 -an $vg1
rm -f "$DF"
dd if=/dev/zero of="$dev1" bs=1M count=1
not vgimportdevices -a
not ls "$DF"
vgs $vg1
pvs "$dev2"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
rm -f "$DF"

# three complete, one incomplete vg
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
vgcreate $vg4 "$dev4" "$dev5"
rm -f "$DF"
dd if=/dev/zero of="$dev5" bs=1M count=1
vgimportdevices -a
ls "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep "$dev3" "$DF"
not grep "$dev4" "$DF"
not grep "$dev5" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
dd if=/dev/zero of="$dev3" bs=1M count=1
dd if=/dev/zero of="$dev4" bs=1M count=1
rm -f "$DF"


#
# vgimportdevices -a with existing df
#

# no devs found
vgcreate $vg1 "$dev1"
grep "$dev1" "$DF"
dd if=/dev/zero of="$dev1" bs=1M count=1
not vgimportdevices -a
ls "$DF"

# one complete vg
vgcreate $vg1 "$dev1"
vgimportdevices -a
grep "$dev1" "$DF"
vgcreate --devicesfile "" $vg2 "$dev2"
not grep "$dev2" "$DF"
vgimportdevices -a
grep "$dev1" "$DF"
grep "$dev2" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
rm -f "$DF"

# two complete vgs
vgcreate --devicesfile "" $vg1 "$dev1"
vgcreate --devicesfile "" $vg2 "$dev2"
rm -f "$DF"
vgimportdevices -a
ls "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
rm -f "$DF"

# one incomplete vg
vgcreate $vg1 "$dev1"
vgimportdevices -a
grep "$dev1" "$DF"
dd if=/dev/zero of="$dev1" bs=1M count=1
vgcreate --devicesfile "" $vg2 "$dev2" "$dev3"
not grep "$dev2" "$DF"
not grep "$dev3" "$DF"
dd if=/dev/zero of="$dev2" bs=1M count=1
not vgimportdevices -a
ls "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
dd if=/dev/zero of="$dev3" bs=1M count=1
rm -f "$DF"

# import the same vg again
vgcreate --devicesfile "" $vg1 "$dev1"
not ls "$DF"
vgimportdevices -a
ls "$DF"
grep "$dev1" "$DF"
vgimportdevices -a
ls "$DF"
grep "$dev1" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
rm -f "$DF"

# import a series of vgs
vgcreate --devicesfile "" $vg1 "$dev1"
vgimportdevices -a
grep "$dev1" "$DF"
vgcreate --devicesfile "" $vg2 "$dev2"
vgimportdevices -a
grep "$dev1" "$DF"
grep "$dev2" "$DF"
vgcreate --devicesfile "" $vg3 "$dev3"
vgimportdevices -a
grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep "$dev3" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
dd if=/dev/zero of="$dev3" bs=1M count=1
rm -f "$DF"

#
# vgimportdevices vg with no prev df
#

# no devs found
not vgimportdevices $vg1
not ls "$DF"

# one complete vg
vgcreate $vg1 "$dev1"
rm -f "$DF"
vgimportdevices $vg1
ls "$DF"
grep "$dev1" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
rm -f "$DF"

# two complete vgs
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
rm -f "$DF"
vgimportdevices $vg1
ls "$DF"
grep "$dev1" "$DF"
vgimportdevices $vg2
ls "$DF"
grep "$dev2" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
rm -f "$DF"

# one incomplete vg
vgcreate $vg1 "$dev1" "$dev2"
lvcreate -l1 -an $vg1
rm -f "$DF"
dd if=/dev/zero of="$dev1" bs=1M count=1
not vgimportdevices $vg1
not ls "$DF"
vgs $vg1
pvs "$dev2"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
rm -f "$DF"

# three complete, one incomplete vg
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
vgcreate $vg4 "$dev4" "$dev5"
rm -f "$DF"
dd if=/dev/zero of="$dev5" bs=1M count=1
not vgimportdevices $vg4
not ls "$DF"
vgimportdevices $vg3
ls "$DF"
grep "$dev3" "$DF"
not grep "$dev1" "$DF"
not grep "$dev2" "$DF"
not grep "$dev4" "$DF"
not grep "$dev5" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
dd if=/dev/zero of="$dev3" bs=1M count=1
dd if=/dev/zero of="$dev4" bs=1M count=1
rm -f "$DF"

#
# vgimportdevices vg with existing df
#

# vg not found
vgcreate $vg1 "$dev1"
vgimportdevices -a
grep "$dev1" "$DF"
not vgimportdevices $vg2
grep "$dev1" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
rm -f "$DF"

# vg incomplete
vgcreate $vg1 "$dev1"
vgimportdevices -a
grep "$dev1" "$DF"
vgcreate --devicesfile "" $vg2 "$dev2" "$dev3"
dd if=/dev/zero of="$dev2" bs=1M count=1
not vgimportdevices $vg2
grep "$dev1" "$DF"
not grep "$dev2" "$DF"
not grep "$dev3" "$DF"

# reset
dd if=/dev/zero of="$dev1" bs=1M count=1
dd if=/dev/zero of="$dev2" bs=1M count=1
dd if=/dev/zero of="$dev3" bs=1M count=1
dd if=/dev/zero of="$dev4" bs=1M count=1
rm -f "$DF"

