#!/usr/bin/env bash

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITHOUT_LVMETAD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_pvs 2

# flip the devices around
init_udev_transaction
dmsetup remove "$dev1"
dmsetup remove "$dev2"
dmsetup create -u TEST-${PREFIX}pv1 ${PREFIX}pv2 ${PREFIX}pv2.table
dmsetup create -u TEST-${PREFIX}pv2 ${PREFIX}pv1 ${PREFIX}pv1.table
finish_udev_transaction
dmsetup info -c

# re-scan them
pvscan --cache "$dev1" || true
pvscan --cache "$dev2" || true

# expect both to be there
pvs -a -o name | tee out
grep "$dev1" out
grep "$dev2" out

aux lvmetad_dump

# flip the devices 2nd. time around
init_udev_transaction
dmsetup remove "$dev1"
dmsetup remove "$dev2"
dmsetup create -u TEST-${PREFIX}pv2 ${PREFIX}pv2 ${PREFIX}pv2.table
dmsetup create -u TEST-${PREFIX}pv1 ${PREFIX}pv1 ${PREFIX}pv1.table
finish_udev_transaction

# re-scan them
pvscan --cache "$dev1" || true
pvscan --cache "$dev2" || true

# expect both to be there
pvs -a -o name | tee out
grep "$dev1" out
grep "$dev2" out

aux lvmetad_dump

# flip the devices 2nd. time around
dmsetup remove -f "$dev1"
dmsetup remove -f "$dev2"
dmsetup create -u TEST-${PREFIX}pv1 ${PREFIX}pv2 ${PREFIX}pv2.table
dmsetup create -u TEST-${PREFIX}pv2 ${PREFIX}pv1 ${PREFIX}pv1.table

# re-scan them
pvscan --cache "$dev1" || true
pvscan --cache "$dev2" || true

# expect both to be there
pvs -a -o name | tee out
grep "$dev1" out
grep "$dev2" out
