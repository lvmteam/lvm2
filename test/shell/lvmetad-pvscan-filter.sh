#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

test -e LOCAL_LVMETAD || skip

aux prepare_pvs 2

maj=$(($(stat --printf=0x%t "$dev2")))
min=$(($(stat --printf=0x%T "$dev2")))

aux hide_dev $dev2
not pvscan --cache $dev2 2>&1 | grep "not found"
# pvscan with --major/--minor does not fail: lvmetad needs to
# be notified about device removal on REMOVE uevent, hence
# this should not fail so udev does not grab a "failed" state
# incorrectly. We notify device addition and removal with
# exactly the same command "pvscan --cache" - in case of removal,
# this is detected by nonexistence of the device itself.
pvscan --cache --major $maj --minor $min 2>&1 | grep "not found"
aux unhide_dev $dev2

pvscan --cache $dev2 2>&1 | not grep "not found"
pvscan --cache --major $maj --minor $min 2>&1 | not grep "not found"
pvs | grep $dev2
