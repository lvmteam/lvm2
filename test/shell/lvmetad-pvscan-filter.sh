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

maj=$(($(stat -L --printf=0x%t "$dev2")))
min=$(($(stat -L --printf=0x%T "$dev2")))

# Filter out device, pvscan should trigger
# clearing of the device from lvmetad cache.

# We can't use aux hide_dev here because that
# changes the global_filter which triggers a
# token mismatch rescan by subsequent pvscan
# commands instead of the single-dev scans
# that are testing here.

mv "$dev2" "$dev2-HIDDEN"

pvscan --cache "$dev2" 2>&1 | tee out || true
grep "not found" out

# pvscan with --major/--minor does not fail: lvmetad needs to
# be notified about device removal on REMOVE uevent, hence
# this should not fail so udev does not grab a "failed" state
# incorrectly. We notify device addition and removal with
# exactly the same command "pvscan --cache" - in case of removal,
# this is detected by nonexistence of the device itself.

pvscan --cache --major $maj --minor $min 2>&1 | tee out || true
grep "not found" out

# aux unhide_dev "$dev2"
mv "$dev2-HIDDEN" "$dev2"

pvscan --cache "$dev2" 2>&1 | tee out || true
not grep "not found" out

pvscan --cache --major $maj --minor $min 2>&1 | tee out || true
not grep "not found" out

pvs | grep "$dev2"
