#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# lvremove must not warn when unmonitoring snapshot LVs whose DM device
# is already gone.

. lib/inittest --skip-with-lvmpolld

aux prepare_dmeventd
aux prepare_vg 2

lvcreate -aey -L16M -n origin $vg
lvcreate -s -L4M -n snap $vg/origin
lvchange --monitor y $vg/snap

lvremove -f $vg >out 2>&1

not grep 'device not found' out
not grep 'Failed to unmonitor' out
