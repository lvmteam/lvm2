#!/usr/bin/env bash

# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
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

# prepare a stats region with a histogram
dmstats create --bounds 10ms,20ms,30ms "$dev1"

# basic dmstats report commands
dmstats report
dmstats report --count 1
dmstats report --histogram

# Reporting a group walks _dm_stats_region_id_disp(), which asks the handle
# for the group descriptor and releases the caller-owned string there.
# Both members carry identical bounds: mixing a histogram region with a
# plain one segfaults in _sum_histogram_bins(), which is a separate
# pre-existing bug.
dmstats create --bounds 10ms,20ms,30ms --start 2048 "$dev1"
dmstats group --alias reportgroup --regions 0,1 "$dev1"
dmstats report
dmstats report -oregion_id,stats_name
dmstats report --histogram
