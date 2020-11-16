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

test_description='hello world for user-defined device list'

. lib/inittest

aux prepare_real_devs

echo "$dev1"

pvcreate "$dev1"

pvs "$dev1"

pvremove "$dev1"

vgcreate $vg "$dev1"

lvcreate -l1 $vg

vgchange -an $vg

vgremove -ff $vg

get_real_devs
for d in "${REAL_DEVICES[@]}"; do
	echo $d
done

