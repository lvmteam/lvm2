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

# tests, write failure on PV1 is not reporting errors on PV2 or PV3...

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

# skip test early if there is no 'delay' target available
aux target_at_least dm-delay 1 1 0 || skip "missing dm-delay target"
touch HAVE_DM_DELAY

#
# Main
#
aux prepare_devs 3 20

pvcreate -y --setphysicalvolumesize 10m "$dev1"
pvcreate "$dev2"
pvcreate "$dev3"

vgcreate $vg "$dev1" "$dev2" "$dev3"

pvs -o +uuid

# Keep device readable, but any write fails
aux writeerror_dev "$dev1" 0:100

# Suppose to fail, size PV1 is not writable
not pvresize "$dev1" 2>&1 | tee out

# Output should not complain about any error on pv2 nor pv3
not grep pv2 out
not grep pv3 out

# Restore working PV1 back
aux enable_dev "$dev1"

# FIXME: Takes a lot of time ATM....
pvck "$dev2"

pvs -o +uuid
vgdisplay

vgremove $vg
