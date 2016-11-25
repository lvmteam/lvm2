#!/bin/sh
# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_CLVMD=1

. lib/inittest

aux prepare_pvs 6

# We need the lvmdbusd.profile for the daemon to utilize JSON
# output
mkdir -p "$TESTDIR/etc/profile"
cp -v "$TESTOLDPWD/lib/lvmdbusd.profile" "$TESTDIR/etc/profile/"

# Need to set this up so that the lvmdbusd service knows which
# binary to be running, which should be the one we just built
export LVM_BINARY=$(which lvm 2>/dev/null)
# skip if we don't have our own lvmetad...
if test -z "${installed_testsuite+varset}"; then
	(echo "$LVM_BINARY" | grep -q "$abs_builddir") || skip
fi

aux prepare_lvmdbusd

$TESTOLDPWD/dbus/lvmdbustest.py -v
