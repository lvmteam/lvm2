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

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

# Don't attempt to test stats with driver < 4.33.00
aux driver_at_least 4 33 || skip

# ensure we can create devices (uses dmsetup, etc)
aux prepare_devs 1

# Create three regions and add them to a group.
dmstats create --start 0 --length 256 "$dev1"
dmstats create --start 256 --length 256 "$dev1"
dmstats create --start 512 --length 256 "$dev1"
dmstats group --alias group0 --regions 0-2 "$dev1"

# Delete a group member out-of-band: the group descriptor stored in the
# group leader's aux_data still refers to the removed region id.
dmsetup message "$dev1" 0 "@stats_delete 1"

# Reading back the group must warn about the stale member id and clear it
# rather than indexing past the end of the region table.
dmstats list "$dev1" -ostats_name |& tee out
grep "contains non-existent region_id" out

# The clear is in-memory only: list never writes the cleaned descriptor
# back to the kernel, so the warning repeats on every subsequent list.
dmstats list "$dev1" -ostats_name |& tee out2
grep "contains non-existent region_id" out2

# Clean up the remaining group and regions.
dmstats delete --groupid 0 "$dev1"
