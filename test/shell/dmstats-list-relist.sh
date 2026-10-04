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

HIST_BOUNDS="10ms,20ms,30ms"

# Successive list invocations must keep reporting the same histogram bounds.
dmstats create --bounds "$HIST_BOUNDS" "$dev1"
dmstats list "$dev1" -ostats_name,hist_bounds |& tee out
grep "$HIST_BOUNDS" out
dmstats list "$dev1" -ostats_name,hist_bounds |& tee out2
grep "$HIST_BOUNDS" out2

dmstats delete --allregions "$dev1"
