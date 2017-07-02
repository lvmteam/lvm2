#!/usr/bin/env bash

# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Test lvm1 format'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 1

if test -n "$LVM_TEST_LVM1" ; then
pvcreate -M1 "$dev1"
vgcreate -M1 $vg "$dev1"
check vg_field $vg fmt "lvm1"
fi

# TODO: if we decide to make using lvm1 with lvmetad an error,
# then if lvmetad is being used, then verify:
# not pvcreate -M1 "$dev1"
# not vgcreate -M1 $vg "$dev1"
#
# TODO: if we decide to allow using lvm1 with lvmetad, but disable lvmetad
# when it happens, then verify:
# pvcreate -M1 "$dev1" | tee err
# grep "disabled" err
# vgcreate -M1 $vg "$dev1" | tee err
# grep "disabled" err

