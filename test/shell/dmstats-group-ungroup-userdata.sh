#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

# Don't attempt to test stats with driver < 4.33.00
aux driver_at_least 4 33 || skip

# ensure we can create devices (uses dmsetup, etc)
aux prepare_devs 1

GROUP_NAME="group0"
USERDATA="foo"

# Create a region and make it part of a group with an alias
dmstats create "$dev1" --userdata "$USERDATA"
dmstats group --alias "$GROUP_NAME" --regions 0 "$dev1"
dmstats list -ostats_name |& tee out
grep "$GROUP_NAME" out
dmstats list -ouserdata |& tee out
grep "$USERDATA" out

# Ungroup the regions then remove them
dmstats ungroup --groupid 0 "$dev1"

# Verify userdata is still valid
dmstats list -ouserdata |& tee out
grep "$USERDATA" out

# Clean up
dmstats delete --allregions "$dev1"
