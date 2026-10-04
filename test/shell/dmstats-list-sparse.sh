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

# Create three regions, delete the middle one, then re-list. The kernel
# still reports region_ids 0 and 2 with a hole at 1; parsing must size
# the region table from the highest id, not only the region count.
dmstats create --start 0 --length 256 "$dev1"
dmstats create --start 256 --length 256 "$dev1"
dmstats create --start 512 --length 256 "$dev1"
dmsetup message "$dev1" 0 "@stats_delete 1"

dmstats list "$dev1" --noheadings --separator : -oregion_id,stats_name |& tee out
grep -E '^0:' out
grep -E '^2:' out
not grep -E '^1:' out

dmstats delete --allregions "$dev1"
